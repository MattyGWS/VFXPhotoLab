#include "ClipboardOperations.h"
#include "ColourConversion.h"
#include "CanvasSizeOperations.h"
#include "AdjustmentPresetStore.h"
#include "VectorAppearancePresetStore.h"
#include "CubeLut.h"
#include "ImageSizeOperations.h"
#include "TrimOperations.h"
#include "CloneStamp.h"
#include "CropOperations.h"
#include "HealingBrush.h"
#include "HistogramService.h"
#include "SpotHealing.h"
#include "SpatialFilter.h"
#include "SmartLayerTileCache.h"
#include "SvgWorkflow.h"
#include "PatchTool.h"
#include "ToneBrush.h"
#include "SmudgeBrush.h"
#include "DocumentSession.h"
#include "DocumentTransformOperations.h"
#include "FillOperations.h"
#include "GradientOperations.h"
#include "LayerMergeOperations.h"
#include "TransformSampling.h"
#include "TransformSafety.h"
#include "ImageProcessor.h"
#include "gpu/ProgressivePreview.h"
#include "gpu/PreviewPublication.h"
#include "gpu/RenderBackend.h"
#include "gpu/TiledCanvasEngine.h"
#include "gpu/TileCache.h"
#include "PhotoDocument.h"
#include "RasterHistory.h"
#include "SessionCache.h"
#include "SelectionHistory.h"
#include "SelectionLocalEditing.h"
#include "SelectionMask.h"
#include "SelectionOperations.h"
#include "SelectionTransformOperations.h"
#include "TgaCodec.h"
#include "TonalMapping.h"
#include "VectorRasterizer.h"

#include <QBuffer>
#include <QColor>
#include <QColorSpace>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QPainter>
#include <QRgba64>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUndoCommand>
#include <QUndoGroup>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

using namespace vfx;

namespace {


QString encodePngBase64(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return {};
    }
    return QString::fromLatin1(bytes.toBase64());
}

bool transformsClose(const QTransform &left, const QTransform &right, const double epsilon = 1.0e-9)
{
    return std::abs(left.m11() - right.m11()) <= epsilon
        && std::abs(left.m12() - right.m12()) <= epsilon
        && std::abs(left.m13() - right.m13()) <= epsilon
        && std::abs(left.m21() - right.m21()) <= epsilon
        && std::abs(left.m22() - right.m22()) <= epsilon
        && std::abs(left.m23() - right.m23()) <= epsilon
        && std::abs(left.m31() - right.m31()) <= epsilon
        && std::abs(left.m32() - right.m32()) <= epsilon
        && std::abs(left.m33() - right.m33()) <= epsilon;
}

bool exactImagesEqual(const QImage &left, const QImage &right)
{
    if (left.isNull() || right.isNull()) {
        return left.isNull() == right.isNull();
    }
    if (left.size() != right.size()
        || left.format() != right.format()
        || left.colorSpace() != right.colorSpace()
        || left.dotsPerMeterX() != right.dotsPerMeterX()
        || left.dotsPerMeterY() != right.dotsPerMeterY()
        || left.devicePixelRatio() != right.devicePixelRatio()
        || left.depth() != right.depth()
        || left.depth() % 8 != 0) {
        return false;
    }
    const qsizetype activeRowBytes = static_cast<qsizetype>(left.width())
        * static_cast<qsizetype>(left.depth() / 8);
    for (int y = 0; y < left.height(); ++y) {
        if (std::memcmp(left.constScanLine(y),
                        right.constScanLine(y),
                        static_cast<std::size_t>(activeRowBytes)) != 0) {
            return false;
        }
    }
    return true;
}

bool imagesWithinChannelTolerance(const QImage &left,
                                  const QImage &right,
                                  const int tolerance)
{
    if (left.isNull() || right.isNull() || left.size() != right.size()) return false;
    const QImage a = left.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = right.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < a.height(); ++y) {
        const uchar *aRow = a.constScanLine(y);
        const uchar *bRow = b.constScanLine(y);
        for (int x = 0; x < a.width() * 4; ++x) {
            if (std::abs(int(aRow[x]) - int(bRow[x])) > tolerance) return false;
        }
    }
    return true;
}

void collectLayerIds(const LayerNode &layer, QSet<QUuid> *ids)
{
    ids->insert(layer.id);
    for (const LayerNode &child : layer.children) {
        collectLayerIds(child, ids);
    }
}

bool mutateLayerObject(QJsonArray *layers,
                       const QUuid &id,
                       const std::function<void(QJsonObject &)> &mutation)
{
    for (int index = 0; index < layers->size(); ++index) {
        QJsonObject object = layers->at(index).toObject();
        if (QUuid(object.value(QStringLiteral("id")).toString()) == id) {
            mutation(object);
            layers->replace(index, object);
            return true;
        }
        QJsonArray children = object.value(QStringLiteral("children")).toArray();
        if (mutateLayerObject(&children, id, mutation)) {
            object.insert(QStringLiteral("children"), children);
            layers->replace(index, object);
            return true;
        }
    }
    return false;
}

void stripSmartTransformMetadata(QJsonArray *layers)
{
    for (qsizetype index = 0; index < layers->size(); ++index) {
        QJsonObject object = layers->at(index).toObject();
        object.remove(QStringLiteral("smartTransform"));
        QJsonArray children = object.value(QStringLiteral("children")).toArray();
        stripSmartTransformMetadata(&children);
        object.insert(QStringLiteral("children"), children);
        layers->replace(index, object);
    }
}

} // namespace

class CoreTests final : public QObject {
    Q_OBJECT

private slots:
    void selectionMaskImplicitStatesStaySparse();
    void selectionShapesCombineWithSoftCoverage();
    void selectionPathsCombineWithSoftCoverage();
    void selectionCoverageImagesRespectCombineModes();
    void selectionEdgeOperationsRemainSparseAndSeamFree();
    void selectionHistoryRoundTripsSparseTiles();
    void selectionClippedRasterErasePreservesHiddenRgb();
    void selectionClippedChannelsPreserveOtherComponents();
    void selectedLayerChannelReferenceIgnoresSiblingRasters();
    void selectionAwareRasterStrokeClipsAndPreservesHiddenRgb();
    void selectionAwareMaskStrokeClipsCoverage();
    void fillCoverageSupportsContiguousAndGlobalMatching();
    void fillCoverageHonoursSelectionAndTransparentEquivalence();
    void fillApplicationPreservesAlphaChannelsMasksAndSixteenBitData();
    void tiledFillCpuPathMatchesDirectReference();
    void gradientGeometryEvaluatesAllModes();
    void gradientApplicationSupportsTargetsSelectionsAndSixteenBit();
    void tiledGradientCpuPathMatchesDirectReference();
    void selectedClearUsesTargetSpecificAlphaSafeValues();
    void selectionDerivedMasksFollowLayerTransformAndStayCompact();
    void clipboardPayloadRoundTripsExactPrivateData();
    void clipboardRgbaExtractionPreservesHiddenRgbAndSoftAlpha();
    void clipboardFullDocumentSelectAllPreservesExactPixels();
    void clipboardChannelExtractionKeepsValuesAndCoverageSeparate();
    void clipboardPasteTargetsOnlyRequestedChannelAndSelection();
    void clipboardMaskPasteRestoresCompactUniformStorage();
    void clipboardDocumentRasterPreservesSelectedRegionScale();
    void clipboardNewDocumentRasterPreservesPixelsProfileAndPrecision();
    void clipboardChannelCopyPasteMovesExactValues();
    void selectedPixelTransformMovesPixelsAndSelectionTogether();
    void projectiveSelectionTransformUsesInverseSampling();
    void projectRoundTripPreservesSelectionInVersionSeven();
    void damagedProjectSelectionIsDiscardedSafely();
    void projectRejectsExcessiveHierarchyAndGuideData();
    void selectionJsonTileLimitArithmeticDoesNotOverflow();
    void documentSessionsOwnIndependentState();
    void smartSourceFoundationPersistsIdentityRevisionsAndRejectsCycles();
    void linkedSmartLayerRoundTripsRelinksReplacesAndEmbeds();
    void linkedSmartLayerPropagatesNestedRevisionsAndRejectsFileCycles();
    void smartLiveFilterFxIntegratedRoundTripPreservesAppearance();
    void smartLayerEmbeddedConversionPreservesStructureAppearanceAndPersistence();
    void smartLayerEditContentsCommitsAndPropagatesDependencies();
    void smartLayerEditContentsPreservesColourManagedComposition();
    void smartLayerTransformsRemainSourceBackedAndPersistSampling();
    void smartLayerLiveFilterStackPersistsOrdersAndCachesCpuStages();
    void layerEffectFoundationPersistsFxStackAndCoverageContract();
    void smartLayerTiledCacheReusesIntermediateTilesAndInvalidatesSelectively();
    void smartLayerCompositeFingerprintIsTileLocal();
    void smartLayerSourceDirtyPropagationKeepsUnaffectedCompositeTiles();
    void smartLayerColdEvictionPurgesRuntimeIntermediates();
    void embeddedSmartSourceSurvivesSessionSnapshot();
    void smartSourceEditorBindingSurvivesSessionSnapshot();
    void documentSessionResetKeepsDocumentButClearsTransientState();
    void documentSessionRenderSerialsAreUniqueAndAdvance();
    void undoGroupRoutesTheActiveSessionStack();
    void sessionSnapshotRoundTripPreservesExactDocumentAndEditorState();
    void damagedSessionSnapshotDoesNotReplaceResidentDocument();
    void residencyManagerEvictsOldestWarmSessionAndRestoresIt();
    void residencyManagerPurgesColdHistoryOnlyUnderHardBudget();
    void tileCacheNamespacesIdenticalSurfacesByDocumentSession();
    void renderBackendRejectsObsoleteSessionWork();
    void renderBackendKeepsDisplayedInfoPerActiveSession();
    void newDocumentCreatesWhiteRgbBackground();
    void editableRasterBaseRoundTripsWithoutDuplicatePayload();
    void appliedCropHistoryUsesSettledToolState();
    void nonDestructiveCropPreservesOffCanvasRasterStorage();
    void unclippedTransformRegionRendersOffCanvasStorage();
    void nonDestructiveCropExpansionKeepsNewCanvasEditable();
    void destructiveCropCopiesStraightHiddenRgbExactly();
    void destructiveCropResetsEmptyRasterReferenceExtent();
    void canvasSizeAnchorsDistributeOddDifferencesDeterministically();
    void canvasSizePureBoundsChangePreservesStoredPixelsAndMasks();
    void canvasSizeTranslatesNestedTreesAtTheRoot();
    void canvasSizeShrinkClipsSelectionAndGuidesWithoutDeletingPixels();
    void canvasSizeRoundTripPreservesOffCanvasStorage();
    void canvasSizePreservesSixteenBitHiddenRgbExactly();
    void canvasSizeColourFillCreatesBottomExtensionLayer();
    void canvasSizeColourFillPreservesSixteenBitHiddenRgb();
    void canvasSizeColourFillHonoursGrayscaleDocuments();
    void canvasSizeColourFillSkipsPureContraction();
    void canvasSizeExtensionLayerRoundTripsVersionSeven();
    void canvasSizeDestructiveClipDeletesRasterAndMaskStorage();
    void canvasSizeDestructiveClipPreservesSixteenBitHiddenRgb();
    void canvasSizeDestructiveSameBoundsCompactsPreservedStorage();
    void canvasSizeCancellationLeavesNoPreparedResult();
    void canvasSizeRejectsUnpersistableSurfaceBeforeAllocation();
    void imageSizeNearestScalesEditableDocumentState();
    void imageSizeScalesNestedCoordinateSystems();
    void imageSizeBilinearPreservesHiddenRgbIndependentlyOfAlpha();
    void imageSizePreservesSixteenBitStraightComponents();
    void imageSizeRoundTripsThroughVersionSevenProject();
    void imageSizeCancellationPublishesNoResult();
    void imageSizeNearestTieUsesExactPixelCentreMapping();
    void imageSizeUsesAcceleratorForEligiblePayloads();
    void imageSizeAcceleratorFailureFallsBackToCpuReference();
    void imageSizeAreaReductionUsesExactBoxAverage();
    void imageSizeAdvancedFiltersPreserveStraightConstantComponents();
    void imageSizeAdvancedMethodsStayOnCpuReference();
    void imageSizeResolutionOnlyPreservesEditablePixels();
    void imageSizeSamePixelSizeNeverRewritesPayloads();
    void imageSizePreflightRejectsCombinedOutputBudgetBeforeAcceleration();
    void imageSizeRejectsUnknownResampleMethod();
    void imageSizePreflightRejectsNonFiniteLayerCoordinates();
    void imageSizeAcceleratorMetadataIsNormalised();
    void imageSizeCancellationAfterGpuReturnPublishesNoResult();
    void imageSizeHandlesEmptyLayerTree();
    void imageSizeStateSurvivesColdResidency();
    void renderedExportPreservesResolutionMetadata();
    void structuralStateReplacementIsAtomic();
    void canvasBoundsStateSurvivesColdResidency();
    void revealAllIncludesHiddenStraightRgbaStorageAndNeverShrinks();
    void fitCanvasToSelectedVectorUsesSemanticBounds();
    void fitCanvasToSelectionUsesExactNonZeroCoverageBounds();
    void fitCanvasToSelectedHiddenGroupUsesFiniteTransformedBounds();
    void fitCanvasToMaskedAdjustmentAndIgnoresUnboundedAdjustment();
    void trimTransparentUsesVisibleCompositeAlphaAndPreservesStorage();
    void trimTransparentCountsSixteenBitAlphaOne();
    void trimTransparentHonoursPassThroughGroupMasks();
    void trimCornerColourUsesToleranceSidesAndTransparentEquivalence();
    void trimCornerColourDestructiveModeClipsExactStorage();
    void automaticTrimCancellationPublishesNoResult();
    void legacyBaseImagePromotesToEditableRaster();
    void emptyEditableLayerTreeRoundTrips();
    void newDocumentGrayscale16RoundTripsSettings();
    void newDocumentRejectsInvalidDimensions();
    void vectorShapeLayerRoundTripsVersionSevenAndRejectsPreVersionSeven();
    void vectorFeatherDataModelDuplicationAndMergeRules();
    void vectorFeatherCpuReferenceSoftensOnlyCombinedCoverage();
    void vectorFeatherGpuPreparationMatchesCpuAndLocalisesTileCache();
    void vectorFeatherWorkflowIntegrationAndSvgRoundTrip();
    void vectorFeatherHardeningAndRegressionCoverage();
    void bezierPathRoundTripsVersionNineAndRejectsPreVersionNine();
    void liveVectorCornersRoundTripBakeAndRejectPreVersionTen();
    void bezierPathInsertionAndNodeModesPreserveCurves();
    void bezierPathSelectedNodeMovementIsAtomic();
    void bezierPathDirectionAndJoiningPreserveGeometry();
    void bezierPathRasterizerExposesSemanticCoverageAndSnapPoints();
    void bezierPathCopyAndImageSizeRegenerateAndScaleNodes();
    void vectorGeometryCachePreservesLongPathTilesAndInvalidatesEdits();
    void vectorRasterizerPreservesResolutionIndependenceAndBitDepth();
    void vectorRoundedCornersStayCircularAndRoundTripIndividually();
    void vectorShapeConversionPreservesVisibleAppearanceAndRoundTrips();
    void expandedVectorStrokesPreserveVisibleGeometryAndRoundTrip();
    void rasterLayerMergePreservesIsolatedCompositeAndHiddenRgb();
    void vectorLayerMergeConvertsShapesAndPreservesAppearance();
    void milestoneIntegrationRoundTripPreservesFillGradientAndMerge();
    void vectorAppearanceRoundTripsSwapsAndApplies();
    void vectorAppearancePresetStoreSavesRenamesAndDeletes();
    void vectorArrowheadsAndArrowShapeRoundTripExpandAndPersist();
    void parameterisedVectorShapesRoundTripAndRejectMalformedPayloads();
    void vectorStrokesRespectAlignmentCapsAndBitDepth();
    void dashedVectorStrokesRoundTripScaleAndRejectPreVersionEleven();
    void vectorSnapPointsExposeSemanticVertices();
    void vectorLayerCopyPreservesWorldPlacementAndRegeneratesIds();
    void imageSizeScalesLineGeometryAndStrokeWithoutRasterising();
    void vectorLayersCompositeThroughMasksGroupsAndTransforms();
    void imageSizeScalesVectorGeometryWithoutRasterising();
    void exposureBrightensImage();
    void adjustmentPreservesCoverage();
    void typedLevelsRoundTripPreservesVersionSevenAndChannels();
    void legacyLevelsJsonMigratesToTypedPayload();
    void levelsPerChannelOutputRangesPreserveAlpha();
    void typedCurvesRoundTripPreservesVersionSevenAndSchemaThree();
    void curvesLookupIsExactForEightAndSixteenBit();
    void upgradedAdjustmentsUseTypedParametersAndPreserveIdentity();
    void schemaOneAdjustmentsMigrateWithProfessionalDefaults();
    void selectiveColourAdjustmentsRoundTripSchemaThree();
    void selectiveColourIdentityPreservesEightAndSixteenBitHiddenRgb();
    void targetedHueRangeAndVibranceRemainSelective();
    void whiteAndColourBalancePreserveAlphaAndNeutralLuminosity();
    void channelAndTonalAdjustmentsRoundTripSchemaFive();
    void channelMixerIdentityMonochromeAndHiddenRgb();
    void blackAndWhiteTargetsColourFamiliesAndPreservesAlpha();
    void gradientMapUsesExactLuminanceLookupAtEightAndSixteenBit();
    void discreteAdjustmentsAreExactAtEightAndSixteenBit();
    void cubeLutParserSupportsOneThreeAndCombinedTables();
    void malformedCubeLutIsRejectedSafely();
    void lutAdjustmentPreservesStrengthAlphaAndPrecision();
    void extendedRangeLutUsesFloatingPointGpuPayload();
    void lutAdjustmentRoundTripsEmbeddedDataInCurrentSchema();
    void builtInAdjustmentPresetsMatchTypeAndSchema();
    void adjustmentPresetStoreRoundTripsUserPreset();
    void shadowsHighlightsRoundTripPreservesVersionSevenAndSchemaSix();
    void shadowsHighlightsIdentityAndRecoveryPreservePrecisionAndAlpha();
    void shadowsHighlightsCancellationPublishesNoPartialResult();
    void shadowsHighlightsTiledRegionsMatchFullRenderAcrossBoundaries();
    void histogramCapturesExactEightAndSixteenBitInput();
    void parallelHistogramReductionIsExactAndDeterministic();
    void histogramSelectionScopeUsesSparseCoverageAndCancellation();
    void adjustmentInputHistogramRespectsGroupBoundaries();
    void hiddenBaseRevealsTransparency();
    void multiplyRasterLayerBlends();
    void translatedRasterLayerMoves();
    void dirtyRegionMatchesFullRender();
    void affineDirtyRegionMatchesFullRender();
    void contentBoundsIgnoreTransparentAndMaskedPixels();
    void maskLifecycleSupportsRasterAdjustmentAndGroupLayers();
    void passThroughGroupAdjustmentAffectsParent();
    void passThroughGroupOpacityAndMaskMixBeforeAfterResults();
    void largePassThroughMixIsParallelSafeAndDeterministic();
    void nestedGroupModesPreserveIsolationBoundaries();
    void deepNestedWorkflowRoundTripPreservesModesAndMasks();
    void cancelledTiledRenderDoesNotPublishObsoleteTiles();
    void renderInfoDescribesVisibleNestedHierarchy();
    void passThroughGroupsUseTiledReferenceWithoutGpu();
    void missingGroupCompositeModeDefaultsToIsolated();
    void movingLayersIntoGroupUpdatesModelImmediately();
    void movingBetweenGroupsPreservesWorldPosition();
    void groupingSelectionCreatesGroupWithChildren();
    void selectedAncestorMovesOnlyOnce();
    void layerNodeEqualityTracksRasterRevisions();
    void tgaRoundTripPreservesPixels();
    void tgaRoundTripPreservesHiddenRgbAtZeroAlpha();
    void alphaSafeFlattenedExportPreservesHiddenRgb();
    void baseOverrideRendersInsteadOfSource();
    void projectSourceRoundTripPreservesHiddenRgb();
    void baseOverrideProjectRoundTripPreservesHiddenRgb();
    void authoritativeFlattenedExportRoundTripsCombinedWorkflow();
    void failedImageExportPreservesExistingFile();
    void projectLoadNormalisesUnexpectedRasterSize();
    void renderBackendResetClearsDisplayedDocumentInfo();
    void projectRoundTripPreservesLayerTree();
    void projectRoundTripPreservesProjectiveGroupTransform();
    void projectRoundTripPreservesPassThroughMode();
    void projectLoadRepairsInvalidGroupCompositeMode();
    void duplicateLayersPreserveMasksAndGenerateFreshIds();
    void duplicateNestedGroupRecursivelyRemapsIds();
    void insertLayerRejectsConflictingDescendantIds();
    void projectLoadRepairsDamagedMaskWithoutLosingLayer();
    void projectLoadNormalisesUnexpectedMaskShapeAndFormat();
    void tileCacheConstructorsUseExpectedBudgets();
    void tileCacheSeparatesResolutionLevels();
    void progressivePreviewChoosesUsefulCoarseLevels();
    void progressivePreviewTilesCoverVisibleRegion();
    void previewPublicationRejectsSupersededInteractiveRequests();
    void interactiveRegionFallbackMatchesTiledRender();
    void interactiveRegionFeatheredVectorMatchesTiledRenderWithoutPersistentTiles();
    void progressiveLevelZeroAssemblyMatchesFullRender();
    void multiresolutionTileMatchesCpuReference();
    void nativeGpuTileRoundTripMatchesCpuWhenAvailable();
    void nativeGpuImageResizeMatchesCpuAcrossTileBoundariesWhenAvailable();
    void nativeGpuAdjustmentsMatchCpuWhenAvailable();
    void nativeGpuLiveFilterStackMatchesCpuWhenAvailable();
    void nativeGpuShadowsHighlightsMatchesCpuAcrossTileBoundariesWhenAvailable();
    void nativeGpuPassThroughMatchesCpuWhenAvailable();
    void nativeHierarchyResourceGuardFallsBackSafelyWhenAvailable();
    void alphaChannelStrokePreservesRgb();
    void rgbaChannelStrokeTouchesOnlySelectedComponent();
    void sixteenBitChannelStrokePreservesOtherComponents();
    void channelTileDeltaRoundTripsExactly();
    void channelTileDeltaUsesComponentStorage();
    void rasterTileDeltaRoundTripsExactly();
    void rasterTileDeltaPreservesStraightRgba64AndHiddenRgb();
    void rasterTileDeltaStoresOnlyChangedTiles();
    void rasterTileDeltaSparseRegionsRoundTrip();
    void rasterTileDeltaTracksBoundaryTiles();
    void rasterTileDeltaHandlesPartialEdgeTile();
    void rasterTileDeltaRestoresNullRaster();
    void rasterTileDeltaRejectsUnexpectedState();
    void rasterTileDeltaRejectsCorruptBoundsAndPayload();
    void maskTileDeltaRestoresCompactMask();
    void maskTileDeltaUsesOneByteStorage();
    void tiledMaskStrokePaintsAndRestoresCoverage();
    void nativeGpuMaskStrokeMatchesCpuWhenAvailable();
    void nativeGpuSelectionAwareRasterStrokeMatchesCpuWhenAvailable();
    void nativeGpuSelectionAwareMaskStrokeMatchesCpuWhenAvailable();
    void cloneStampCopiesStraightRgbaAndHiddenRgb();
    void cloneStampSoftTransparencyAvoidsColourHalos();
    void cloneStampLowOpacityOverlapAvoidsChromaticContours();
    void cloneStampTargetsChannelsAndMasks();
    void cloneStampHonoursTransformsAndOutsideSource();
    void cloneStampPreservesSixteenBitComponents();
    void cloneStampTiledCpuFallbackMatchesReference();
    void nativeGpuCloneStampMatchesCpuWhenAvailable();
    void healingBrushTransfersDetailAndAdaptsColour();
    void healingBrushSeamlesslyRemovesDestinationStep();
    void healingBrushPreservesDestinationAlphaAndHiddenRgb();
    void healingBrushPreservesSixteenBitAlpha();
    void healingBrushOutsideSourceIsNoOp();
    void spotHealingChoosesDeterministicNonOverlappingSource();
    void spotHealingPreservesDestinationAlpha();
    void patchToolSourceModeReplacesSelectedStructure();
    void patchToolDestinationModeMovesSourceAndPreservesAlpha();
    void patchToolPreservesSixteenBitAlpha();
    void retouchCancellationNeverPublishesPartialResult();
    void dodgeBurnRespectRangesAndPreserveRasterAlpha();
    void dodgeBurnTargetComponentChannelsAndMasks();
    void spongeChangesChromaWhilePreservingLuminanceAndAlpha();
    void toneBrushPreservesSixteenBitAlpha();
    void toneBrushHonoursLinearSrgbEncoding();
    void toneBrushIncrementalPreviewMatchesWholeGesture();
    void blurSharpenPreserveAlphaAndHiddenRgbEdges();
    void blurSharpenTargetChannelsMasksAndMatchIncrementalPreview();
    void smudgeBrushTransportsStraightRgbaAndFingerColour();
    void smudgeBrushTargetsSelectionsChannelsMasksAndMatchesIncremental();
    void smudgeBrushLongIncrementalStrokeReusesBoundedScratch();
    void layerPlacementSupportsCreatedRasterRedo();
    void orthogonalTransformPreservesStraightHiddenRgb();
    void orthogonalDocumentTransformUpdatesSelectionGuidesAndResolution();
    void orthogonalDocumentTransformRepeatsWithoutAspectScaling();
    void transformSamplingHonoursNearestAndLanczos();
    void transformSafetyAcceptsBoundedOffCanvasStorage();
    void transformSafetyRejectsProjectiveHorizonsAndOversizedPayloads();
    void clipboardPasteDistinguishesSafeEmptyFromFailure();
    void clipboardRasterTransformPreservesHiddenRgbAtZeroAlpha();
    void layerJsonRepairsUnsafeTransformMetadata();
    void layerJsonRejectsExcessiveHierarchyAndRasterEncoding();
    void structuralReplacementRejectsUnsafeTransformMetadata();
    void transformWorkflowSurvivesColdResidency();
};



void CoreTests::selectionMaskImplicitStatesStaySparse()
{
    SelectionMask selection(QSize(1024, 768));
    QVERIFY(!selection.isActive());
    QCOMPARE(selection.explicitTileCount(), 0);

    selection.selectAll();
    QVERIFY(selection.isActive());
    QVERIFY(selection.isFull());
    QCOMPARE(selection.explicitTileCount(), 0);
    QCOMPARE(selection.nonZeroBounds(), QRect(0, 0, 1024, 768));
    QCOMPARE(selection.coverageAt(0, 0), static_cast<quint8>(255));
    QCOMPARE(selection.coverageAt(1023, 767), static_cast<quint8>(255));

    selection.selectNone();
    QVERIFY(selection.isActive());
    QVERIFY(selection.isEmpty());
    QCOMPARE(selection.explicitTileCount(), 0);
    QCOMPARE(selection.coverageAt(400, 300), static_cast<quint8>(0));

    bool jsonOk = false;
    const QJsonObject emptyJson = selection.toJson(&jsonOk);
    QVERIFY(jsonOk);
    QString selectionWarning;
    SelectionMask restoredEmpty = SelectionMask::fromJson(
        emptyJson, selection.size(), &jsonOk, &selectionWarning);
    QVERIFY2(jsonOk, qPrintable(selectionWarning));
    QVERIFY(restoredEmpty.isActive());
    QVERIFY(restoredEmpty.isEmpty());
    QCOMPARE(restoredEmpty.explicitTileCount(), 0);

    QVERIFY(selection.setCoverageRect(QRect(250, 250, 20, 20), 173));
    QVERIFY(!selection.isEmpty());
    QCOMPARE(selection.explicitTileCount(), 4);
    QCOMPARE(selection.nonZeroBounds(), QRect(250, 250, 20, 20));
    QCOMPARE(selection.coverageAt(250, 250), static_cast<quint8>(173));
    QCOMPARE(selection.coverageAt(269, 269), static_cast<quint8>(173));
    QCOMPARE(selection.coverageAt(249, 249), static_cast<quint8>(0));

    selection.deactivate();
    QVERIFY(!selection.isActive());
    QCOMPARE(selection.explicitTileCount(), 0);
}

void CoreTests::selectionShapesCombineWithSoftCoverage()
{
    SelectionMask selection(QSize(128, 96));
    QVERIFY(selection.combineShape(QRectF(10.0, 12.0, 30.0, 20.0),
                                   SelectionShape::Rectangle,
                                   SelectionCombineMode::Replace,
                                   false));
    QVERIFY(selection.isActive());
    QCOMPARE(selection.coverageAt(20, 20), static_cast<quint8>(255));
    QCOMPARE(selection.coverageAt(5, 5), static_cast<quint8>(0));

    QVERIFY(selection.combineShape(QRectF(35.0, 12.0, 25.0, 20.0),
                                   SelectionShape::Rectangle,
                                   SelectionCombineMode::Add,
                                   false));
    QCOMPARE(selection.coverageAt(50, 20), static_cast<quint8>(255));
    QCOMPARE(selection.coverageAt(20, 20), static_cast<quint8>(255));

    QVERIFY(selection.combineShape(QRectF(18.0, 0.0, 6.0, 96.0),
                                   SelectionShape::Rectangle,
                                   SelectionCombineMode::Subtract,
                                   false));
    QCOMPARE(selection.coverageAt(20, 20), static_cast<quint8>(0));
    QCOMPARE(selection.coverageAt(30, 20), static_cast<quint8>(255));

    QVERIFY(selection.combineShape(QRectF(25.25, 8.25, 30.5, 30.5),
                                   SelectionShape::Ellipse,
                                   SelectionCombineMode::Intersect,
                                   true));
    QCOMPARE(selection.coverageAt(5, 5), static_cast<quint8>(0));
    QCOMPARE(selection.coverageAt(40, 22), static_cast<quint8>(255));

    bool foundSoftEdge = false;
    for (int y = 0; y < selection.size().height() && !foundSoftEdge; ++y) {
        for (int x = 0; x < selection.size().width(); ++x) {
            const quint8 coverage = selection.coverageAt(x, y);
            if (coverage > 0 && coverage < 255) {
                foundSoftEdge = true;
                break;
            }
        }
    }
    QVERIFY(foundSoftEdge);
    QVERIFY(selection.explicitTileCount() <= 1);

    QVERIFY(selection.combineShape(QRectF(-50.0, -50.0, 250.0, 200.0),
                                   SelectionShape::Rectangle,
                                   SelectionCombineMode::Replace,
                                   false));
    QVERIFY(selection.isFull());
    QCOMPARE(selection.explicitTileCount(), 0);
    QVERIFY(!selection.combineShape(QRectF(12.0, 12.0, 20.0, 20.0),
                                    SelectionShape::Rectangle,
                                    SelectionCombineMode::Add,
                                    false));
    QVERIFY(selection.isFull());

    selection.deactivate();
    QVERIFY(selection.combineShape(QRectF(15.0, 15.0, 20.0, 20.0),
                                   SelectionShape::Rectangle,
                                   SelectionCombineMode::Subtract,
                                   false));
    QVERIFY(selection.isActive());
    QVERIFY(selection.isEmpty());
}

void CoreTests::selectionPathsCombineWithSoftCoverage()
{
    SelectionMask selection(QSize(160, 120));
    QPainterPath freehand;
    freehand.moveTo(18.25, 18.5);
    freehand.lineTo(86.5, 12.25);
    freehand.lineTo(118.75, 62.5);
    freehand.lineTo(72.25, 101.75);
    freehand.lineTo(20.5, 75.25);
    freehand.closeSubpath();
    QVERIFY(selection.combinePath(freehand,
                                  SelectionCombineMode::Replace,
                                  true));
    QVERIFY(selection.isActive());
    QCOMPARE(selection.coverageAt(65, 55), static_cast<quint8>(255));
    QCOMPARE(selection.coverageAt(145, 105), static_cast<quint8>(0));

    bool foundSoftEdge = false;
    for (int y = 0; y < selection.size().height() && !foundSoftEdge; ++y) {
        for (int x = 0; x < selection.size().width(); ++x) {
            const quint8 coverage = selection.coverageAt(x, y);
            if (coverage > 0 && coverage < 255) {
                foundSoftEdge = true;
                break;
            }
        }
    }
    QVERIFY(foundSoftEdge);

    QPainterPath polygon;
    polygon.moveTo(55.0, 30.0);
    polygon.lineTo(145.0, 30.0);
    polygon.lineTo(145.0, 92.0);
    polygon.lineTo(55.0, 92.0);
    polygon.closeSubpath();
    QVERIFY(selection.combinePath(polygon,
                                  SelectionCombineMode::Add,
                                  false));
    QCOMPARE(selection.coverageAt(130, 60), static_cast<quint8>(255));

    QPainterPath cutout;
    cutout.addEllipse(QRectF(62.0, 42.0, 36.0, 36.0));
    QVERIFY(selection.combinePath(cutout,
                                  SelectionCombineMode::Subtract,
                                  false));
    QCOMPARE(selection.coverageAt(80, 60), static_cast<quint8>(0));
    QCOMPARE(selection.coverageAt(130, 60), static_cast<quint8>(255));

    const SelectionMask::Snapshot before = selection.snapshot();
    QPainterPath outside;
    outside.addRect(QRectF(300.0, 300.0, 20.0, 20.0));
    QVERIFY(selection.combinePath(outside,
                                  SelectionCombineMode::Intersect,
                                  false));
    QVERIFY(selection.isActive());
    QVERIFY(selection.isEmpty());
    const SelectionTileDeltaSet delta = buildSelectionTileDeltaSet(
        before, selection.snapshot());
    QVERIFY(!delta.isEmpty());
    QVERIFY(applySelectionTileDeltaSet(&selection, delta, false));
    QCOMPARE(selection.coverageAt(130, 60), static_cast<quint8>(255));
}

void CoreTests::selectionCoverageImagesRespectCombineModes()
{
    SelectionMask selection(QSize(64, 48));
    QImage source(QSize(20, 16), QImage::Format_Grayscale8);
    source.fill(0);
    for (int y = 2; y < 14; ++y) {
        uchar *line = source.scanLine(y);
        for (int x = 3; x < 17; ++x) {
            line[x] = static_cast<uchar>(x == 3 ? 128 : 255);
        }
    }

    QVERIFY(selection.combineCoverageImage(QRect(10, 8, 20, 16),
                                           source,
                                           SelectionCombineMode::Replace));
    QCOMPARE(selection.coverageAt(14, 12), static_cast<quint8>(255));
    QCOMPARE(selection.coverageAt(13, 12), static_cast<quint8>(128));
    QCOMPARE(selection.coverageAt(5, 5), static_cast<quint8>(0));

    QImage add(QSize(8, 8), QImage::Format_Grayscale8);
    add.fill(200);
    QVERIFY(selection.combineCoverageImage(QRect(28, 20, 8, 8),
                                           add,
                                           SelectionCombineMode::Add));
    QCOMPARE(selection.coverageAt(31, 23), static_cast<quint8>(200));
    QCOMPARE(selection.coverageAt(14, 12), static_cast<quint8>(255));

    QImage cut(QSize(4, 4), QImage::Format_Grayscale8);
    cut.fill(255);
    QVERIFY(selection.combineCoverageImage(QRect(12, 10, 4, 4),
                                           cut,
                                           SelectionCombineMode::Subtract));
    QCOMPARE(selection.coverageAt(14, 12), static_cast<quint8>(0));

    QImage overlap(QSize(6, 6), QImage::Format_Grayscale8);
    overlap.fill(255);
    QVERIFY(selection.combineCoverageImage(QRect(28, 20, 6, 6),
                                           overlap,
                                           SelectionCombineMode::Intersect));
    QCOMPARE(selection.coverageAt(31, 23), static_cast<quint8>(200));
    QCOMPARE(selection.coverageAt(20, 18), static_cast<quint8>(0));
    QCOMPARE(selection.nonZeroBounds(), QRect(28, 20, 6, 6));
}

void CoreTests::selectionEdgeOperationsRemainSparseAndSeamFree()
{
    SelectionMask selection(QSize(768, 512));
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(220, 120, 160, 180), 255));

    SelectionMask::Snapshot expanded;
    QVERIFY(SelectionOperations::expand(selection, 12, &expanded));
    SelectionMask expandedSelection(selection.size());
    QVERIFY(expandedSelection.restoreSnapshot(expanded, false));
    QCOMPARE(expandedSelection.nonZeroBounds(), QRect(208, 108, 184, 204));
    QCOMPARE(expandedSelection.coverageAt(208, 150), static_cast<quint8>(255));
    QCOMPARE(expandedSelection.coverageAt(207, 150), static_cast<quint8>(0));

    SelectionMask::Snapshot contracted;
    QVERIFY(SelectionOperations::contract(expandedSelection, 12, &contracted));
    SelectionMask contractedSelection(selection.size());
    QVERIFY(contractedSelection.restoreSnapshot(contracted, false));
    QCOMPARE(contractedSelection.nonZeroBounds(), QRect(220, 120, 160, 180));

    SelectionMask::Snapshot feathered;
    QVERIFY(SelectionOperations::feather(selection, 10, &feathered));
    SelectionMask featheredSelection(selection.size());
    QVERIFY(featheredSelection.restoreSnapshot(feathered, false));
    QVERIFY(featheredSelection.nonZeroBounds().left() < selection.nonZeroBounds().left());
    QVERIFY(featheredSelection.coverageAt(220, 170) > 0);
    QVERIFY(featheredSelection.coverageAt(220, 170) < 255);
    // The rectangle crosses the x=256 tile boundary. Local halo processing
    // must not introduce a discontinuity between adjacent output tiles.
    QCOMPARE(featheredSelection.coverageAt(255, 170),
             featheredSelection.coverageAt(256, 170));

    SelectionRefineParameters identity;
    SelectionMask::Snapshot unchanged;
    QVERIFY(!SelectionOperations::refine(featheredSelection,
                                         identity,
                                         &unchanged));
    QCOMPARE(unchanged.active, feathered.active);
    QCOMPARE(unchanged.implicitCoverage, feathered.implicitCoverage);
    QVERIFY(unchanged.tiles == feathered.tiles);

    SelectionMask full(QSize(1024, 768));
    full.selectAll();
    SelectionMask::Snapshot fullFeathered;
    QVERIFY(SelectionOperations::feather(full, 8, &fullFeathered));
    SelectionMask fullResult(full.size());
    QVERIFY(fullResult.restoreSnapshot(fullFeathered, false));
    QVERIFY(fullResult.implicitCoverage() == 255);
    QVERIFY(fullResult.explicitTileCount() > 0);
    QVERIFY(fullResult.explicitTileCount() <= 10);
    QCOMPARE(fullResult.coverageAt(512, 384), static_cast<quint8>(255));
    QVERIFY(fullResult.coverageAt(0, 0) < 255);

    SelectionMask::Snapshot emptied;
    QVERIFY(SelectionOperations::contract(selection, 200, &emptied));
    SelectionMask emptyResult(selection.size());
    QVERIFY(emptyResult.restoreSnapshot(emptied, false));
    QVERIFY(emptyResult.isActive());
    QVERIFY(emptyResult.isEmpty());
    QCOMPARE(emptyResult.explicitTileCount(), 0);

    QImage refineSource(20, 20, QImage::Format_Grayscale8);
    refineSource.fill(0);
    for (int y = 5; y < 15; ++y) {
        std::fill_n(refineSource.scanLine(y) + 5, 10, static_cast<uchar>(255));
    }
    SelectionRefineParameters shiftOut;
    shiftOut.shiftEdgePercent = 100;
    const QImage expandedPreview = SelectionOperations::refineCoverageImage(
        refineSource, shiftOut, 1.0);
    QVERIFY(!expandedPreview.isNull());
    QCOMPARE(expandedPreview.constScanLine(10)[4], static_cast<uchar>(255));

    SelectionRefineParameters shiftIn;
    shiftIn.shiftEdgePercent = -100;
    const QImage contractedPreview = SelectionOperations::refineCoverageImage(
        refineSource, shiftIn, 1.0);
    QVERIFY(!contractedPreview.isNull());
    QCOMPARE(contractedPreview.constScanLine(10)[5], static_cast<uchar>(0));
    QCOMPARE(contractedPreview.constScanLine(10)[6], static_cast<uchar>(255));
}

void CoreTests::selectionHistoryRoundTripsSparseTiles()
{
    SelectionMask selection(QSize(1024, 768));
    selection.selectNone();
    const SelectionMask::Snapshot before = selection.snapshot();
    QVERIFY(selection.setCoverageRect(QRect(250, 250, 20, 20), 211));
    const SelectionMask::Snapshot after = selection.snapshot();

    const SelectionTileDeltaSet delta = buildSelectionTileDeltaSet(before, after);
    QVERIFY(!delta.isEmpty());
    QCOMPARE(delta.tiles.size(), 4);
    QVERIFY(delta.storedBytes() < 4 * 256 * 256);

    QVERIFY(applySelectionTileDeltaSet(&selection, delta, false));
    QVERIFY(selection.isActive());
    QVERIFY(selection.isEmpty());
    QCOMPARE(selection.explicitTileCount(), 0);

    QVERIFY(applySelectionTileDeltaSet(&selection, delta, true));
    QCOMPARE(selection.nonZeroBounds(), QRect(250, 250, 20, 20));
    QCOMPARE(selection.coverageAt(260, 260), static_cast<quint8>(211));

    const SelectionMask::Snapshot sparseBefore = selection.snapshot();
    selection.selectAll();
    const SelectionTileDeltaSet selectAllDelta = buildSelectionTileDeltaSet(
        sparseBefore, selection.snapshot());
    QVERIFY(!selectAllDelta.isEmpty());
    QCOMPARE(selection.explicitTileCount(), 0);
    QVERIFY(applySelectionTileDeltaSet(&selection, selectAllDelta, false));
    QCOMPARE(selection.coverageAt(260, 260), static_cast<quint8>(211));
    QCOMPARE(selection.coverageAt(0, 0), static_cast<quint8>(0));

    selection.selectAll();
    const SelectionMask::Snapshot fullBefore = selection.snapshot();
    // This intersection covers four complete 256x256 tiles. Their actual
    // bytes are 255 before and after; only their explicit/implicit
    // representation changes, so history must retain zero-XOR tile records.
    QVERIFY(selection.combineShape(QRectF(0.0, 0.0, 512.0, 512.0),
                                   SelectionShape::Rectangle,
                                   SelectionCombineMode::Intersect,
                                   false));
    QCOMPARE(selection.explicitTileCount(), 4);
    const SelectionMask::Snapshot partialAfter = selection.snapshot();
    const SelectionTileDeltaSet implicitRepresentationDelta =
        buildSelectionTileDeltaSet(fullBefore, partialAfter);
    QVERIFY(!implicitRepresentationDelta.isEmpty());
    QCOMPARE(implicitRepresentationDelta.tiles.size(), 4);
    QVERIFY(applySelectionTileDeltaSet(&selection,
                                       implicitRepresentationDelta,
                                       false));
    QVERIFY(selection.isFull());
    QVERIFY(applySelectionTileDeltaSet(&selection,
                                       implicitRepresentationDelta,
                                       true));
    QCOMPARE(selection.coverageAt(150, 150), static_cast<quint8>(255));
    QCOMPARE(selection.coverageAt(700, 700), static_cast<quint8>(0));
}

void CoreTests::selectionClippedRasterErasePreservesHiddenRgb()
{
    const QSize size(4, 1);
    SelectionMask selection(size);
    selection.selectNone();
    QImage coverage(size, QImage::Format_Grayscale8);
    coverage.fill(0);
    coverage.scanLine(0)[1] = 255;
    coverage.scanLine(0)[2] = 128;
    QVERIFY(selection.setCoverageImage(QRect(QPoint(0, 0), size), coverage));

    QImage source(size, QImage::Format_RGBA8888);
    QImage erased(size, QImage::Format_RGBA8888);
    for (int x = 0; x < size.width(); ++x) {
        source.setPixelColor(x, 0, QColor(20 + x, 40 + x, 60 + x, 255));
        // Deliberately destroy the ordinary eraser result's RGB. The
        // selection clip must still recover source hidden RGB at zero alpha.
        erased.setPixelColor(x, 0, QColor(0, 0, 0, 0));
    }

    QVERIFY(clipEditedImageToSelection(&erased,
                                       source,
                                       QRect(QPoint(0, 0), size),
                                       selection.snapshot(),
                                       QTransform(),
                                       SelectionEditKind::RasterPixels));
    QCOMPARE(erased.pixelColor(0, 0), source.pixelColor(0, 0));
    QCOMPARE(erased.pixelColor(1, 0).red(), source.pixelColor(1, 0).red());
    QCOMPARE(erased.pixelColor(1, 0).green(), source.pixelColor(1, 0).green());
    QCOMPARE(erased.pixelColor(1, 0).blue(), source.pixelColor(1, 0).blue());
    QCOMPARE(erased.pixelColor(1, 0).alpha(), 0);
    QCOMPARE(erased.pixelColor(2, 0).red(), source.pixelColor(2, 0).red());
    QCOMPARE(erased.pixelColor(2, 0).green(), source.pixelColor(2, 0).green());
    QCOMPARE(erased.pixelColor(2, 0).blue(), source.pixelColor(2, 0).blue());
    QVERIFY(std::abs(erased.pixelColor(2, 0).alpha() - 127) <= 1);
    QCOMPARE(erased.pixelColor(3, 0), source.pixelColor(3, 0));

    SelectionMask transformedSelection(size);
    transformedSelection.selectNone();
    QVERIFY(transformedSelection.setCoverageRect(QRect(2, 0, 1, 1), 255));
    QImage transformedErase(size, QImage::Format_RGBA8888);
    transformedErase.fill(Qt::transparent);
    QTransform translatedLayer;
    translatedLayer.translate(1.0, 0.0);
    QVERIFY(clipEditedImageToSelection(&transformedErase,
                                       source,
                                       transformedErase.rect(),
                                       transformedSelection.snapshot(),
                                       translatedLayer,
                                       SelectionEditKind::RasterPixels));
    QCOMPARE(transformedErase.pixelColor(0, 0), source.pixelColor(0, 0));
    QCOMPARE(transformedErase.pixelColor(1, 0).red(), source.pixelColor(1, 0).red());
    QCOMPARE(transformedErase.pixelColor(1, 0).green(), source.pixelColor(1, 0).green());
    QCOMPARE(transformedErase.pixelColor(1, 0).blue(), source.pixelColor(1, 0).blue());
    QCOMPARE(transformedErase.pixelColor(1, 0).alpha(), 0);
    QCOMPARE(transformedErase.pixelColor(2, 0), source.pixelColor(2, 0));
    QCOMPARE(transformedErase.pixelColor(3, 0), source.pixelColor(3, 0));

    SelectionMask activeEmpty(size);
    activeEmpty.selectNone();
    QImage blockedErase(size, QImage::Format_RGBA8888);
    blockedErase.fill(Qt::transparent);
    QVERIFY(clipEditedImageToSelection(&blockedErase,
                                       source,
                                       blockedErase.rect(),
                                       activeEmpty.snapshot(),
                                       QTransform(),
                                       SelectionEditKind::RasterPixels));
    QVERIFY(exactImagesEqual(blockedErase, source));
}

void CoreTests::selectionClippedChannelsPreserveOtherComponents()
{
    const QSize size(3, 1);
    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(1, 0, 1, 1), 255));

    QImage source(size, QImage::Format_RGBA8888);
    source.fill(QColor(10, 20, 30, 40));
    QImage edited(size, QImage::Format_RGBA8888);
    edited.fill(QColor(200, 201, 202, 203));
    QVERIFY(clipEditedImageToSelection(&edited,
                                       source,
                                       edited.rect(),
                                       selection.snapshot(),
                                       QTransform(),
                                       SelectionEditKind::ComponentChannel,
                                       0));
    QCOMPARE(edited.pixelColor(0, 0), source.pixelColor(0, 0));
    QCOMPARE(edited.pixelColor(1, 0), QColor(200, 20, 30, 40));
    QCOMPARE(edited.pixelColor(2, 0), source.pixelColor(2, 0));

    edited.fill(QColor(100, 110, 120, 130));
    QVERIFY(clipEditedImageToSelection(&edited,
                                       source,
                                       edited.rect(),
                                       selection.snapshot(),
                                       QTransform(),
                                       SelectionEditKind::GreyChannel));
    QCOMPARE(edited.pixelColor(0, 0), source.pixelColor(0, 0));
    QCOMPARE(edited.pixelColor(1, 0), QColor(100, 110, 120, 40));
    QCOMPARE(edited.pixelColor(2, 0), source.pixelColor(2, 0));

    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(255);
    QImage paintedMask(size, QImage::Format_Grayscale8);
    paintedMask.fill(0);
    QVERIFY(clipEditedImageToSelection(&paintedMask,
                                       mask,
                                       paintedMask.rect(),
                                       selection.snapshot(),
                                       QTransform(),
                                       SelectionEditKind::Mask));
    QCOMPARE(paintedMask.constScanLine(0)[0], static_cast<uchar>(255));
    QCOMPARE(paintedMask.constScanLine(0)[1], static_cast<uchar>(0));
    QCOMPARE(paintedMask.constScanLine(0)[2], static_cast<uchar>(255));

    QImage source16(size, QImage::Format_RGBA64);
    QImage edited16(size, QImage::Format_RGBA64);
    auto *source16Row = reinterpret_cast<QRgba64 *>(source16.scanLine(0));
    auto *edited16Row = reinterpret_cast<QRgba64 *>(edited16.scanLine(0));
    for (int x = 0; x < size.width(); ++x) {
        source16Row[x] = QRgba64::fromRgba64(1000, 2000, 3000, 4000);
        edited16Row[x] = QRgba64::fromRgba64(5000, 6000, 7000, 8000);
    }
    QVERIFY(clipEditedImageToSelection(&edited16,
                                       source16,
                                       edited16.rect(),
                                       selection.snapshot(),
                                       QTransform(),
                                       SelectionEditKind::ComponentChannel,
                                       1));
    const auto *clipped16 = reinterpret_cast<const QRgba64 *>(edited16.constScanLine(0));
    QCOMPARE(clipped16[0].red(), static_cast<quint16>(1000));
    QCOMPARE(clipped16[0].green(), static_cast<quint16>(2000));
    QCOMPARE(clipped16[0].blue(), static_cast<quint16>(3000));
    QCOMPARE(clipped16[0].alpha(), static_cast<quint16>(4000));
    QCOMPARE(clipped16[1].red(), static_cast<quint16>(1000));
    QCOMPARE(clipped16[1].green(), static_cast<quint16>(6000));
    QCOMPARE(clipped16[1].blue(), static_cast<quint16>(3000));
    QCOMPARE(clipped16[1].alpha(), static_cast<quint16>(4000));
    QCOMPARE(clipped16[2].red(), static_cast<quint16>(1000));
    QCOMPARE(clipped16[2].green(), static_cast<quint16>(2000));
    QCOMPARE(clipped16[2].blue(), static_cast<quint16>(3000));
    QCOMPARE(clipped16[2].alpha(), static_cast<quint16>(4000));
}


void CoreTests::selectedLayerChannelReferenceIgnoresSiblingRasters()
{
    const QSize size(48, 32);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4] = static_cast<uchar>(20 + x);
            row[x * 4 + 1] = static_cast<uchar>(40 + y);
            row[x * 4 + 2] = static_cast<uchar>(80 + (x + y) % 60);
            row[x * 4 + 3] = static_cast<uchar>((x + y) % 5 == 0 ? 0 : 210);
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.name = QStringLiteral("Base Image");
    LayerNode paint;
    paint.type = LayerType::Raster;
    paint.name = QStringLiteral("Paint Layer");
    paint.rasterImage = QImage(size, QImage::Format_RGBA8888);
    paint.rasterImage.fill(QColor(0, 0, 0, 0));
    // A visible painted spot proves the sibling exists while its transparent
    // hidden RGB must never replace the selected Base Image channel values.
    {
        uchar *pixel = paint.rasterImage.scanLine(4) + 7 * 4;
        pixel[0] = 255;
        pixel[1] = 0;
        pixel[2] = 0;
        pixel[3] = 255;
    }
    const QVector<LayerNode> layers {paint, base};

    const QImage rgbReference = ImageProcessor::renderLayerRegionChannelReference(
        source, layers, base.id, source.rect(), size, true);
    QVERIFY(!rgbReference.isNull());
    QCOMPARE(rgbReference.size(), size);
    const QImage rgb = rgbReference.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *expected = source.constScanLine(y);
        const uchar *actual = rgb.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(actual[x * 4], expected[x * 4]);
            QCOMPARE(actual[x * 4 + 1], expected[x * 4 + 1]);
            QCOMPARE(actual[x * 4 + 2], expected[x * 4 + 2]);
            QCOMPARE(actual[x * 4 + 3], static_cast<uchar>(255));
        }
    }

    const QImage alphaReference = ImageProcessor::renderLayerRegionChannelReference(
        source, layers, base.id, source.rect(), size, false);
    QVERIFY(!alphaReference.isNull());
    const QImage alpha = alphaReference.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *expected = source.constScanLine(y);
        const uchar *actual = alpha.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(actual[x * 4 + 3], expected[x * 4 + 3]);
        }
    }

    LayerNode emptyRaster;
    emptyRaster.type = LayerType::Raster;
    const QImage emptyReference = ImageProcessor::renderLayerRegionChannelReference(
        source, {emptyRaster, paint, base}, emptyRaster.id, source.rect(), size, true);
    QVERIFY(!emptyReference.isNull());
    const QImage emptyRgba = emptyReference.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < emptyRgba.height(); ++y) {
        const uchar *row = emptyRgba.constScanLine(y);
        for (int x = 0; x < emptyRgba.width(); ++x) {
            QCOMPARE(row[x * 4], static_cast<uchar>(0));
            QCOMPARE(row[x * 4 + 1], static_cast<uchar>(0));
            QCOMPARE(row[x * 4 + 2], static_cast<uchar>(0));
            QCOMPARE(row[x * 4 + 3], static_cast<uchar>(255));
        }
    }
}

void CoreTests::selectionAwareRasterStrokeClipsAndPreservesHiddenRgb()
{
    const QSize size(320, 180);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4] = 23;
            row[x * 4 + 1] = 77;
            row[x * 4 + 2] = 141;
            row[x * 4 + 3] = 255;
        }
    }

    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(92, 30, 136, 120), 255));
    QVERIFY(selection.setCoverageRect(QRect(84, 30, 8, 120), 128));
    const QVector<QLineF> segments {
        QLineF(QPointF(20.0, 90.0), QPointF(300.0, 90.0))
    };
    const SelectionMask::Snapshot snapshot = selection.snapshot();
    TiledCanvasEngine engine;
    const auto painted = engine.stampRasterStroke(
        source, size, source.colorSpace(), QUuid::createUuid(), 1,
        segments, QTransform(), 54.0, 1.0, 0.92,
        QColor(240, 30, 15, 255), false, false, QUuid(),
        &snapshot, QTransform());
    QVERIFY2(!painted.image.isNull(), qPrintable(painted.error));
    QVERIFY(painted.selectionApplied);
    const QImage output = painted.image.convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output.pixelColor(40, 90), source.pixelColor(40, 90));
    QVERIFY(output.pixelColor(150, 90).red() > 220);
    QVERIFY(output.pixelColor(88, 90).red() > source.pixelColor(88, 90).red());
    QVERIFY(output.pixelColor(88, 90).red() < output.pixelColor(150, 90).red());

    const auto erased = engine.stampRasterStroke(
        source, size, source.colorSpace(), QUuid::createUuid(), 2,
        {QLineF(QPointF(150.0, 90.0), QPointF(150.0, 90.0))},
        QTransform(), 70.0, 1.0, 0.999, QColor(255, 255, 255, 255),
        true, false, QUuid(), &snapshot, QTransform());
    QVERIFY2(!erased.image.isNull(), qPrintable(erased.error));
    const QImage erasedRgba = erased.image.convertToFormat(QImage::Format_RGBA8888);
    const uchar *centre = erasedRgba.constScanLine(90) + 150 * 4;
    QCOMPARE(centre[3], static_cast<uchar>(0));
    QCOMPARE(centre[0], static_cast<uchar>(23));
    QCOMPARE(centre[1], static_cast<uchar>(77));
    QCOMPARE(centre[2], static_cast<uchar>(141));
    QCOMPARE(erasedRgba.pixelColor(40, 90), source.pixelColor(40, 90));
}

void CoreTests::selectionAwareMaskStrokeClipsCoverage()
{
    const QSize size(420, 220);
    QImage source(1, 1, QImage::Format_Grayscale8);
    source.fill(255);
    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(110, 30, 190, 160), 255));
    QVERIFY(selection.setCoverageRect(QRect(100, 30, 10, 160), 128));
    const SelectionMask::Snapshot snapshot = selection.snapshot();

    TiledCanvasEngine engine;
    const auto result = engine.stampMaskStroke(
        source, size, QUuid::createUuid(), 1,
        {QLineF(QPointF(20.0, 110.0), QPointF(400.0, 110.0))},
        QTransform(), 48.0, 1.0, 0.9, 0, false, false, QUuid(),
        &snapshot, QTransform());
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    QVERIFY(result.selectionApplied);
    QCOMPARE(result.image.constScanLine(110)[60], static_cast<uchar>(255));
    QVERIFY(result.image.constScanLine(110)[180] < 8);
    const int soft = result.image.constScanLine(110)[105];
    QVERIFY(soft > 90 && soft < 180);
    QCOMPARE(result.image.constScanLine(110)[350], static_cast<uchar>(255));
}

void CoreTests::selectedClearUsesTargetSpecificAlphaSafeValues()
{
    const QSize size(3, 1);
    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(1, 0, 1, 1), 255));

    QImage source(size, QImage::Format_RGBA8888);
    source.fill(QColor(80, 90, 100, 200));
    QImage result;
    QRect affected;
    QVERIFY(clearImageThroughSelection(&result,
                                       source,
                                       selection.snapshot(),
                                       size,
                                       QTransform(),
                                       SelectionEditKind::RasterPixels,
                                       -1,
                                       &affected));
    QCOMPARE(affected, QRect(1, 0, 1, 1));
    QCOMPARE(result.pixelColor(1, 0), QColor(80, 90, 100, 0));
    QCOMPARE(result.pixelColor(0, 0), source.pixelColor(0, 0));

    QVERIFY(clearImageThroughSelection(&result,
                                       source,
                                       selection.snapshot(),
                                       size,
                                       QTransform(),
                                       SelectionEditKind::ComponentChannel,
                                       1,
                                       &affected));
    QCOMPARE(result.pixelColor(1, 0), QColor(80, 0, 100, 200));

    QVERIFY(clearImageThroughSelection(&result,
                                       source,
                                       selection.snapshot(),
                                       size,
                                       QTransform(),
                                       SelectionEditKind::ComponentChannel,
                                       3,
                                       &affected));
    QCOMPARE(result.pixelColor(1, 0), QColor(80, 90, 100, 0));

    QVERIFY(clearImageThroughSelection(&result,
                                       source,
                                       selection.snapshot(),
                                       size,
                                       QTransform(),
                                       SelectionEditKind::GreyChannel,
                                       -1,
                                       &affected));
    QCOMPARE(result.pixelColor(1, 0), QColor(0, 0, 0, 200));

    QImage compactMask(1, 1, QImage::Format_Grayscale8);
    compactMask.fill(255);
    QVERIFY(clearImageThroughSelection(&result,
                                       compactMask,
                                       selection.snapshot(),
                                       size,
                                       QTransform(),
                                       SelectionEditKind::Mask,
                                       -1,
                                       &affected));
    QCOMPARE(result.size(), size);
    QCOMPARE(result.constScanLine(0)[0], static_cast<uchar>(255));
    QCOMPARE(result.constScanLine(0)[1], static_cast<uchar>(0));
    QCOMPARE(result.constScanLine(0)[2], static_cast<uchar>(255));

    QImage source16(size, QImage::Format_RGBA64);
    for (int x = 0; x < size.width(); ++x) {
        auto *row = reinterpret_cast<QRgba64 *>(source16.scanLine(0));
        row[x] = QRgba64::fromRgba64(1234, 2345, 3456, 45678);
    }
    QVERIFY(clearImageThroughSelection(&result,
                                       source16,
                                       selection.snapshot(),
                                       size,
                                       QTransform(),
                                       SelectionEditKind::RasterPixels,
                                       -1,
                                       &affected));
    const auto *result16 = reinterpret_cast<const QRgba64 *>(result.constScanLine(0));
    QCOMPARE(result16[1].red(), static_cast<quint16>(1234));
    QCOMPARE(result16[1].green(), static_cast<quint16>(2345));
    QCOMPARE(result16[1].blue(), static_cast<quint16>(3456));
    QCOMPARE(result16[1].alpha(), static_cast<quint16>(0));
}

void CoreTests::selectionDerivedMasksFollowLayerTransformAndStayCompact()
{
    const QSize size(4, 2);
    SelectionMask full(size);
    full.selectAll();
    QImage mask = selectionAsLayerMask(full, QTransform(), size);
    QCOMPARE(mask.size(), QSize(1, 1));
    QCOMPARE(mask.constScanLine(0)[0], static_cast<uchar>(255));

    QTransform translatedFullTransform;
    translatedFullTransform.translate(2.0, 0.0);
    QImage translatedFullMask = selectionAsLayerMask(
        full, translatedFullTransform, size);
    QCOMPARE(translatedFullMask.size(), size);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *row = translatedFullMask.constScanLine(y);
        QCOMPARE(row[0], static_cast<uchar>(255));
        QCOMPARE(row[1], static_cast<uchar>(255));
        QCOMPARE(row[2], static_cast<uchar>(0));
        QCOMPARE(row[3], static_cast<uchar>(0));
    }
    QTransform offCanvasTransform;
    offCanvasTransform.translate(20.0, 0.0);
    const QImage offCanvasMask = selectionAsLayerMask(full, offCanvasTransform, size);
    QCOMPARE(offCanvasMask.size(), QSize(1, 1));
    QCOMPARE(offCanvasMask.constScanLine(0)[0], static_cast<uchar>(0));

    SelectionMask empty(size);
    empty.selectNone();
    mask = selectionAsLayerMask(empty, QTransform(), size);
    QCOMPARE(mask.size(), QSize(1, 1));
    QCOMPARE(mask.constScanLine(0)[0], static_cast<uchar>(0));

    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(2, 0, 2, 2), 255));
    QTransform layerToDocument;
    layerToDocument.translate(2.0, 0.0);
    mask = selectionAsLayerMask(selection, layerToDocument, size);
    QCOMPARE(mask.size(), size);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        QCOMPARE(row[0], static_cast<uchar>(255));
        QCOMPARE(row[1], static_cast<uchar>(255));
        QCOMPARE(row[2], static_cast<uchar>(0));
        QCOMPARE(row[3], static_cast<uchar>(0));
    }
}



void CoreTests::clipboardPayloadRoundTripsExactPrivateData()
{
    ClipboardPayload payload;
    payload.imageKind = ClipboardImageKind::Rgba;
    payload.sourceKind = ClipboardSourceKind::RasterPixels;
    payload.image = QImage(QSize(3, 2), QImage::Format_RGBA64);
    payload.image.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    for (int y = 0; y < payload.image.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(payload.image.scanLine(y));
        for (int x = 0; x < payload.image.width(); ++x) {
            row[x] = QRgba64::fromRgba64(1000 + x, 2000 + y, 3000 + x + y,
                                         static_cast<quint16>(4000 + x * 50 + y));
        }
    }
    payload.documentBounds = QRect(17, 23, 3, 2);
    payload.sourceDocumentSize = QSize(640, 480);
    payload.hasDocumentPlacement = true;
    payload.sourceName = QStringLiteral("Layer A");
    QVERIFY(payload.isValid());

    bool encoded = false;
    const QByteArray bytes = payload.toBytes(&encoded);
    QVERIFY(encoded);
    QVERIFY(!bytes.isEmpty());
    QString error;
    const std::optional<ClipboardPayload> decoded = ClipboardPayload::fromBytes(bytes, &error);
    QVERIFY2(decoded.has_value(), qPrintable(error));
    QCOMPARE(decoded->imageKind, payload.imageKind);
    QCOMPARE(decoded->sourceKind, payload.sourceKind);
    QCOMPARE(decoded->documentBounds, payload.documentBounds);
    QCOMPARE(decoded->sourceDocumentSize, payload.sourceDocumentSize);
    QCOMPARE(decoded->hasDocumentPlacement, payload.hasDocumentPlacement);
    QCOMPARE(decoded->sourceName, payload.sourceName);
    QVERIFY(exactImagesEqual(decoded->image, payload.image));

    QByteArray damaged = bytes;
    damaged[damaged.size() - 1] = static_cast<char>(damaged.back() ^ 0x5a);
    error.clear();
    QVERIFY(!ClipboardPayload::fromBytes(damaged, &error).has_value());
    QVERIFY(!error.isEmpty());
}

void CoreTests::clipboardRgbaExtractionPreservesHiddenRgbAndSoftAlpha()
{
    QImage source(QSize(4, 3), QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    uchar *pixel = source.scanLine(1) + 2 * 4;
    pixel[0] = 210;
    pixel[1] = 80;
    pixel[2] = 35;
    pixel[3] = 0;
    uchar *opaque = source.scanLine(1) + 1 * 4;
    opaque[0] = 20;
    opaque[1] = 100;
    opaque[2] = 240;
    opaque[3] = 200;

    SelectionMask selection(source.size());
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(1, 1, 1, 1), 128));
    QVERIFY(selection.setCoverageRect(QRect(2, 1, 1, 1), 255));
    const ClipboardPayload payload = extractClipboardPayload(
        source, source.size(), QTransform(), source.size(), selection.snapshot(),
        ClipboardSourceKind::RasterPixels, -1, QStringLiteral("Pixels"));
    QVERIFY(payload.isValid());
    QCOMPARE(payload.documentBounds, QRect(1, 1, 2, 1));
    const QImage rgba = payload.image.convertToFormat(QImage::Format_RGBA8888);
    const uchar *row = rgba.constScanLine(0);
    QCOMPARE(row[0], static_cast<uchar>(20));
    QCOMPARE(row[1], static_cast<uchar>(100));
    QCOMPARE(row[2], static_cast<uchar>(240));
    QCOMPARE(row[3], static_cast<uchar>((200 * 128 + 127) / 255));
    QCOMPARE(row[4], static_cast<uchar>(210));
    QCOMPARE(row[5], static_cast<uchar>(80));
    QCOMPARE(row[6], static_cast<uchar>(35));
    QCOMPARE(row[7], static_cast<uchar>(0));
}

void CoreTests::clipboardFullDocumentSelectAllPreservesExactPixels()
{
    const QSize size(17, 11);
    QImage source(size, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4] = static_cast<uchar>((x * 17 + y * 3) & 255);
            row[x * 4 + 1] = static_cast<uchar>((x * 5 + y * 19) & 255);
            row[x * 4 + 2] = static_cast<uchar>((x * 23 + y * 7) & 255);
            row[x * 4 + 3] = static_cast<uchar>((x + y) % 4 == 0 ? 0 : 255);
        }
    }
    SelectionMask selection(size);
    selection.selectAll();
    const ClipboardPayload payload = extractClipboardPayload(
        source, size, QTransform(), size, selection.snapshot(),
        ClipboardSourceKind::RasterPixels, -1, QStringLiteral("Full image"));
    QVERIFY(payload.isValid());
    QCOMPARE(payload.documentBounds, QRect(QPoint(0, 0), size));
    QVERIFY(exactImagesEqual(payload.image, source));

    const QImage pasted = clipboardPayloadAsDocumentRaster(
        payload, source.colorSpace(), false, false, size);
    QVERIFY(exactImagesEqual(pasted, source));
}

void CoreTests::clipboardChannelExtractionKeepsValuesAndCoverageSeparate()
{
    QImage source(QSize(3, 1), QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    for (int x = 0; x < 3; ++x) {
        uchar *pixel = source.scanLine(0) + x * 4;
        pixel[0] = static_cast<uchar>(30 + x * 40);
        pixel[1] = 5;
        pixel[2] = 7;
        pixel[3] = 255;
    }
    SelectionMask selection(source.size());
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(0, 0, 1, 1), 64));
    QVERIFY(selection.setCoverageRect(QRect(1, 0, 1, 1), 255));
    const ClipboardPayload payload = extractClipboardPayload(
        source, source.size(), QTransform(), source.size(), selection.snapshot(),
        ClipboardSourceKind::RedChannel, 0, QStringLiteral("Red"));
    QVERIFY(payload.isValid());
    QCOMPARE(payload.imageKind, ClipboardImageKind::Grayscale);
    QCOMPARE(payload.documentBounds, QRect(0, 0, 2, 1));
    const QImage values = payload.image.convertToFormat(QImage::Format_Grayscale8);
    const QImage coverage = payload.coverage.convertToFormat(QImage::Format_Grayscale8);
    QCOMPARE(values.constScanLine(0)[0], static_cast<uchar>(30));
    QCOMPARE(values.constScanLine(0)[1], static_cast<uchar>(70));
    QCOMPARE(coverage.constScanLine(0)[0], static_cast<uchar>(64));
    QCOMPARE(coverage.constScanLine(0)[1], static_cast<uchar>(255));
}

void CoreTests::clipboardPasteTargetsOnlyRequestedChannelAndSelection()
{
    ClipboardPayload payload;
    payload.imageKind = ClipboardImageKind::Rgba;
    payload.sourceKind = ClipboardSourceKind::ExternalImage;
    payload.image = QImage(QSize(2, 1), QImage::Format_RGBA8888);
    payload.image.fill(QColor(255, 255, 255, 255));
    payload.documentBounds = QRect(1, 0, 2, 1);
    payload.sourceDocumentSize = QSize(4, 1);
    payload.hasDocumentPlacement = true;
    QVERIFY(payload.isValid());

    QImage target(QSize(4, 1), QImage::Format_RGBA8888);
    for (int x = 0; x < target.width(); ++x) {
        uchar *pixel = target.scanLine(0) + x * 4;
        pixel[0] = 10;
        pixel[1] = 20;
        pixel[2] = 30;
        pixel[3] = 40;
    }
    SelectionMask selection(target.size());
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(2, 0, 1, 1), 128));
    const ClipboardPasteResult result = pasteClipboardIntoTarget(
        payload, target, target.size(), QTransform(), target.size(),
        ClipboardPasteTarget::RedChannel, selection.snapshot());
    QVERIFY(result.succeeded);
    QVERIFY(result.changed);
    QCOMPARE(result.affectedRect, QRect(2, 0, 1, 1));
    const QImage output = result.image.convertToFormat(QImage::Format_RGBA8888);
    for (int x = 0; x < output.width(); ++x) {
        const uchar *pixel = output.constScanLine(0) + x * 4;
        QCOMPARE(pixel[1], static_cast<uchar>(20));
        QCOMPARE(pixel[2], static_cast<uchar>(30));
        QCOMPARE(pixel[3], static_cast<uchar>(40));
        if (x == 2) {
            QCOMPARE(pixel[0], static_cast<uchar>((10 * 127 + 255 * 128 + 127) / 255));
        } else {
            QCOMPARE(pixel[0], static_cast<uchar>(10));
        }
    }
}

void CoreTests::clipboardMaskPasteRestoresCompactUniformStorage()
{
    ClipboardPayload payload;
    payload.imageKind = ClipboardImageKind::Grayscale;
    payload.sourceKind = ClipboardSourceKind::Mask;
    payload.image = QImage(QSize(4, 4), QImage::Format_Grayscale8);
    payload.image.fill(0);
    payload.coverage = QImage(QSize(4, 4), QImage::Format_Grayscale8);
    payload.coverage.fill(255);
    payload.documentBounds = QRect(0, 0, 4, 4);
    payload.sourceDocumentSize = QSize(4, 4);
    payload.hasDocumentPlacement = true;
    QVERIFY(payload.isValid());

    QImage compactWhite(QSize(1, 1), QImage::Format_Grayscale8);
    compactWhite.fill(255);
    SelectionMask noSelection(QSize(4, 4));
    const ClipboardPasteResult result = pasteClipboardIntoTarget(
        payload, compactWhite, QSize(4, 4), QTransform(), QSize(4, 4),
        ClipboardPasteTarget::Mask, noSelection.snapshot());
    QVERIFY(result.succeeded);
    QVERIFY(result.changed);
    QCOMPARE(result.image.size(), QSize(1, 1));
    QCOMPARE(qGray(result.image.pixel(0, 0)), 0);
    QCOMPARE(result.affectedRect, QRect(0, 0, 4, 4));
}

void CoreTests::clipboardDocumentRasterPreservesSelectedRegionScale()
{
    ClipboardPayload payload;
    payload.imageKind = ClipboardImageKind::Rgba;
    payload.sourceKind = ClipboardSourceKind::RasterPixels;
    payload.image = QImage(2, 2, QImage::Format_RGBA8888);
    payload.image.fill(QColor(220, 30, 10, 255));
    payload.documentBounds = QRect(3, 4, 2, 2);
    payload.sourceDocumentSize = QSize(10, 10);
    payload.hasDocumentPlacement = true;

    const QImage raster = clipboardPayloadAsDocumentRaster(
        payload, QColorSpace(QColorSpace::SRgb), false, false, QSize(10, 10));
    QCOMPARE(raster.size(), QSize(10, 10));
    QCOMPARE(QColor::fromRgba(raster.pixel(3, 4)), QColor(220, 30, 10, 255));
    QCOMPARE(QColor::fromRgba(raster.pixel(4, 5)), QColor(220, 30, 10, 255));
    QCOMPARE(qAlpha(raster.pixel(2, 4)), 0);
    QCOMPARE(qAlpha(raster.pixel(5, 5)), 0);
}

void CoreTests::clipboardNewDocumentRasterPreservesPixelsProfileAndPrecision()
{
    ClipboardPayload payload;
    payload.imageKind = ClipboardImageKind::Rgba;
    payload.sourceKind = ClipboardSourceKind::ExternalImage;
    payload.image = QImage(QSize(3, 2), QImage::Format_RGBA64);
    payload.image.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    payload.image.setDotsPerMeterX(4'724);
    payload.image.setDotsPerMeterY(5'906);
    payload.image.setDevicePixelRatio(2.0);
    for (int y = 0; y < payload.image.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(payload.image.scanLine(y));
        for (int x = 0; x < payload.image.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(1'000 + x * 3 + y),
                static_cast<quint16>(20'000 + x * 5 + y),
                static_cast<quint16>(60'000 - x * 7 - y),
                static_cast<quint16>((x == 1 && y == 0) ? 0 : 48'000));
        }
    }
    payload.documentBounds = QRect(QPoint(0, 0), payload.image.size());
    payload.sourceDocumentSize = payload.image.size();
    payload.hasDocumentPlacement = false;
    payload.sourceName = QStringLiteral("Clipboard Image");
    QVERIFY(payload.isValid());

    QString error;
    const QImage result = clipboardPayloadAsNewDocumentRaster(payload, &error);
    QVERIFY2(!result.isNull(), qPrintable(error));
    QCOMPARE(result.size(), payload.image.size());
    QCOMPARE(result.format(), QImage::Format_RGBA64);
    QCOMPARE(result.colorSpace(), payload.image.colorSpace());
    QCOMPARE(result.dotsPerMeterX(), payload.image.dotsPerMeterX());
    QCOMPARE(result.dotsPerMeterY(), payload.image.dotsPerMeterY());
    QCOMPARE(result.devicePixelRatio(), 1.0);
    const auto *sourcePixels = reinterpret_cast<const QRgba64 *>(payload.image.constBits());
    const auto *resultPixels = reinterpret_cast<const QRgba64 *>(result.constBits());
    for (int index = 0; index < payload.image.width() * payload.image.height(); ++index) {
        QCOMPARE(resultPixels[index], sourcePixels[index]);
    }

    PhotoDocument clipboardDocument;
    clipboardDocument.setSourceImage(result);
    QCOMPARE(clipboardDocument.sourceImage().size(), payload.image.size());
    QCOMPARE(clipboardDocument.sourceImage().format(), QImage::Format_RGBA64);
    QCOMPARE(clipboardDocument.sourceImage().colorSpace(), payload.image.colorSpace());
    QCOMPARE(clipboardDocument.layers().size(), 1);
    QCOMPARE(clipboardDocument.layers().constFirst().rasterImage, result);

    ClipboardPayload scalar16;
    scalar16.imageKind = ClipboardImageKind::Grayscale;
    scalar16.sourceKind = ClipboardSourceKind::GreyChannel;
    scalar16.image = QImage(QSize(2, 1), QImage::Format_Grayscale16);
    scalar16.coverage = QImage(QSize(2, 1), QImage::Format_Grayscale16);
    auto *scalarValues = reinterpret_cast<quint16 *>(scalar16.image.bits());
    auto *scalarCoverage = reinterpret_cast<quint16 *>(scalar16.coverage.bits());
    scalarValues[0] = 1'234;
    scalarValues[1] = 54'321;
    scalarCoverage[0] = 0;
    scalarCoverage[1] = 45'678;
    scalar16.image.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    scalar16.documentBounds = QRect(QPoint(0, 0), scalar16.image.size());
    scalar16.sourceDocumentSize = scalar16.image.size();
    QVERIFY(scalar16.isValid());
    const QImage scalarResult = clipboardPayloadAsNewDocumentRaster(scalar16, &error);
    QVERIFY2(!scalarResult.isNull(), qPrintable(error));
    QCOMPARE(scalarResult.format(), QImage::Format_RGBA64);
    const auto *scalarPixels = reinterpret_cast<const QRgba64 *>(scalarResult.constBits());
    QCOMPARE(scalarPixels[0], QRgba64::fromRgba64(1'234, 1'234, 1'234, 0));
    QCOMPARE(scalarPixels[1], QRgba64::fromRgba64(54'321, 54'321, 54'321, 45'678));
    QCOMPARE(scalarResult.colorSpace(), QColorSpace(QColorSpace::SRgbLinear));

    ClipboardPayload untagged = payload;
    untagged.image.setColorSpace(QColorSpace());
    const QImage assigned = clipboardPayloadAsNewDocumentRaster(untagged, &error);
    QVERIFY2(!assigned.isNull(), qPrintable(error));
    QCOMPARE(assigned.colorSpace(), QColorSpace(QColorSpace::SRgb));

    ClipboardPayload eightBit;
    eightBit.imageKind = ClipboardImageKind::Rgba;
    eightBit.sourceKind = ClipboardSourceKind::ExternalImage;
    eightBit.image = QImage(QSize(2, 1), QImage::Format_RGBA8888);
    eightBit.image.setPixelColor(0, 0, QColor(19, 73, 211, 0));
    eightBit.image.setPixelColor(1, 0, QColor(241, 8, 91, 173));
    eightBit.documentBounds = QRect(QPoint(0, 0), eightBit.image.size());
    eightBit.sourceDocumentSize = eightBit.image.size();
    QVERIFY(eightBit.isValid());
    const QImage exactEightBit = clipboardPayloadAsNewDocumentRaster(eightBit, &error);
    QVERIFY2(!exactEightBit.isNull(), qPrintable(error));
    QCOMPARE(exactEightBit.format(), QImage::Format_RGBA8888);
    QCOMPARE(exactEightBit.pixelColor(0, 0), QColor(19, 73, 211, 0));
    QCOMPARE(exactEightBit.pixelColor(1, 0), QColor(241, 8, 91, 173));

    ClipboardPayload tooWide = eightBit;
    tooWide.image = QImage(QSize(32'769, 1), QImage::Format_RGBA8888);
    tooWide.image.fill(Qt::transparent);
    tooWide.documentBounds = QRect(QPoint(0, 0), tooWide.image.size());
    tooWide.sourceDocumentSize = tooWide.image.size();
    QVERIFY(tooWide.isValid());
    QVERIFY(clipboardPayloadAsNewDocumentRaster(tooWide, &error).isNull());
    QVERIFY(error.contains(QStringLiteral("dimensions"), Qt::CaseInsensitive));
}

void CoreTests::clipboardChannelCopyPasteMovesExactValues()
{
    QImage source(4, 2, QImage::Format_RGBA8888);
    source.fill(QColor(0, 7, 11, 255));
    source.setPixelColor(1, 0, QColor(180, 7, 11, 255));
    source.setPixelColor(2, 0, QColor(90, 7, 11, 255));

    SelectionMask selection(source.size());
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(1, 0, 2, 1), 255));
    const ClipboardPayload copied = extractClipboardPayload(
        source, source.size(), QTransform(), source.size(), selection.snapshot(),
        ClipboardSourceKind::RedChannel, 0, QStringLiteral("Red"));
    QVERIFY(copied.isValid());
    QCOMPARE(copied.imageKind, ClipboardImageKind::Grayscale);

    SelectionMask destination(source.size());
    destination.deactivate();
    const ClipboardPasteResult pasted = pasteClipboardIntoTarget(
        copied, source, source.size(), QTransform(), source.size(),
        ClipboardPasteTarget::GreenChannel, destination.snapshot(), false,
        QTransform::fromTranslate(1.0, 1.0));
    QVERIFY(pasted.succeeded);
    QVERIFY(pasted.changed);
    QCOMPARE(pasted.image.pixelColor(2, 1).green(), 180);
    QCOMPARE(pasted.image.pixelColor(3, 1).green(), 90);
    QCOMPARE(pasted.image.pixelColor(2, 1).red(), 0);
    QCOMPARE(pasted.image.pixelColor(2, 1).blue(), 11);
}

void CoreTests::selectedPixelTransformMovesPixelsAndSelectionTogether()
{
    const QSize size(8, 8);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setPixelColor(2, 2, QColor(210, 20, 30, 255));
    source.setPixelColor(3, 2, QColor(20, 210, 30, 255));

    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(2, 2, 2, 1), 255));
    const ClipboardPayload payload = extractClipboardPayload(
        source, size, QTransform(), size, selection.snapshot(),
        ClipboardSourceKind::RasterPixels, -1, QStringLiteral("Pixels"));
    QVERIFY(payload.isValid());

    QImage cleared;
    QVERIFY(clearImageThroughSelection(&cleared, source, selection.snapshot(),
                                       size, QTransform(), SelectionEditKind::RasterPixels,
                                       -1));
    const QTransform move = QTransform::fromTranslate(2.0, 3.0);
    const ClipboardPasteResult pasted = pasteClipboardIntoRasterTarget(
        payload, cleared, size, QTransform(), size, false, move);
    QVERIFY(pasted.succeeded);
    QVERIFY(pasted.changed);
    QCOMPARE(qAlpha(pasted.image.pixel(2, 2)), 0);
    QCOMPARE(pasted.image.pixelColor(4, 5), QColor(210, 20, 30, 255));
    QCOMPARE(pasted.image.pixelColor(5, 5), QColor(20, 210, 30, 255));

    const SelectionMask::Snapshot moved = transformedSelectionSnapshot(
        selection.snapshot(), move, size);
    SelectionMask restored(size);
    QVERIFY(restored.restoreSnapshot(moved, false));
    QCOMPARE(restored.nonZeroBounds(), QRect(4, 5, 2, 1));
    QCOMPARE(restored.coverageAt(4, 5), static_cast<quint8>(255));
    QCOMPARE(restored.coverageAt(2, 2), static_cast<quint8>(0));
}

void CoreTests::projectiveSelectionTransformUsesInverseSampling()
{
    const QSize size(64, 64);
    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(16, 16, 24, 24), 255));

    QPolygonF source;
    source << QPointF(16.0, 16.0) << QPointF(40.0, 16.0)
           << QPointF(40.0, 40.0) << QPointF(16.0, 40.0);
    QPolygonF target;
    target << QPointF(20.0, 18.0) << QPointF(37.0, 14.0)
           << QPointF(44.0, 43.0) << QPointF(13.0, 39.0);
    QTransform projective;
    QVERIFY(QTransform::quadToQuad(source, target, projective));
    QCOMPARE(projective.type(), QTransform::TxProject);

    const SelectionMask::Snapshot transformed = transformedSelectionSnapshot(
        selection.snapshot(), projective, size);
    SelectionMask restored(size);
    QVERIFY(restored.restoreSnapshot(transformed, false));
    QVERIFY(restored.isActive());
    QVERIFY(!restored.isEmpty());
    QVERIFY(restored.coverageAt(28, 28) > 220);
    QCOMPARE(restored.coverageAt(4, 4), static_cast<quint8>(0));
    QVERIFY(restored.nonZeroBounds().contains(QPoint(28, 28)));
}

void CoreTests::projectRoundTripPreservesSelectionInVersionSeven()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString path = temporaryDirectory.filePath(QStringLiteral("selection.vfxphoto"));

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(640, 480);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(252, 126, 300, 220), 196));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(json.isObject());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);
    QVERIFY(json.object().value(QStringLiteral("selection")).isObject());

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(restored.selectionMask().isActive());
    QCOMPARE(restored.selectionMask().nonZeroBounds(), QRect(252, 126, 300, 220));
    QCOMPARE(restored.selectionMask().coverageAt(300, 200), static_cast<quint8>(196));
    QCOMPARE(restored.selectionMask().coverageAt(10, 10), static_cast<quint8>(0));
    QVERIFY(restored.loadWarnings().isEmpty());
    QVERIFY(!restored.isModified());
}

void CoreTests::damagedProjectSelectionIsDiscardedSafely()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString path = temporaryDirectory.filePath(QStringLiteral("damaged-selection.vfxphoto"));

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(300, 200);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(20, 30, 80, 70), 255));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = json.object();
    QJsonObject selection = root.value(QStringLiteral("selection")).toObject();
    QJsonArray tiles = selection.value(QStringLiteral("tiles")).toArray();
    QVERIFY(!tiles.isEmpty());
    QJsonObject firstTile = tiles.at(0).toObject();
    firstTile.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
    tiles.replace(0, firstTile);
    selection.insert(QStringLiteral("tiles"), tiles);
    root.insert(QStringLiteral("selection"), selection);

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(!restored.selectionMask().isActive());
    QVERIFY(!restored.loadWarnings().isEmpty());
    QVERIFY(restored.isModified());
}

void CoreTests::projectRejectsExcessiveHierarchyAndGuideData()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bounded-project.vfxphoto"));

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(32, 24);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument validDocument = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(validDocument.isObject());
    const QJsonObject validRoot = validDocument.object();

    LayerNode leaf;
    leaf.type = LayerType::Raster;
    leaf.name = QStringLiteral("Leaf");
    bool leafOk = false;
    QJsonObject nested = leaf.toJson(&leafOk);
    QVERIFY(leafOk);
    for (int depth = 0; depth < LayerNode::MaximumTreeDepth; ++depth) {
        LayerNode group;
        group.type = LayerType::Group;
        group.name = QStringLiteral("Nested %1").arg(depth);
        bool groupOk = false;
        QJsonObject parent = group.toJson(&groupOk);
        QVERIFY(groupOk);
        parent.insert(QStringLiteral("children"), QJsonArray {nested});
        nested = std::move(parent);
    }

    QJsonObject excessiveTreeRoot = validRoot;
    excessiveTreeRoot.insert(QStringLiteral("layerTree"), QJsonArray {nested});
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(excessiveTreeRoot).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    PhotoDocument rejectedTree;
    error.clear();
    QVERIFY(!rejectedTree.loadProject(path, &error));
    QVERIFY(error.contains(QStringLiteral("hierarchy"), Qt::CaseInsensitive)
            || error.contains(QStringLiteral("safety"), Qt::CaseInsensitive));
    QVERIFY(!rejectedTree.hasImage());

    QJsonObject excessiveGuidesRoot = validRoot;
    QJsonArray guides;
    for (int index = 0; index <= 65536; ++index) guides.append(0.0);
    QJsonObject guidesObject;
    guidesObject.insert(QStringLiteral("horizontal"), guides);
    guidesObject.insert(QStringLiteral("vertical"), QJsonArray());
    excessiveGuidesRoot.insert(QStringLiteral("guides"), guidesObject);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(excessiveGuidesRoot).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    PhotoDocument rejectedGuides;
    error.clear();
    QVERIFY(!rejectedGuides.loadProject(path, &error));
    QVERIFY(error.contains(QStringLiteral("guide"), Qt::CaseInsensitive));
    QVERIFY(!rejectedGuides.hasImage());

    QJsonObject invalidGuideTypeRoot = validRoot;
    invalidGuideTypeRoot.insert(QStringLiteral("guides"), QStringLiteral("not-an-object"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(file.write(QJsonDocument(invalidGuideTypeRoot).toJson(QJsonDocument::Compact)) > 0);
    file.close();

    PhotoDocument rejectedGuideType;
    error.clear();
    QVERIFY(!rejectedGuideType.loadProject(path, &error));
    QVERIFY(error.contains(QStringLiteral("guide"), Qt::CaseInsensitive));
    QVERIFY(!rejectedGuideType.hasImage());
}

void CoreTests::selectionJsonTileLimitArithmeticDoesNotOverflow()
{
    SelectionMask compact(QSize(1, 1));
    compact.selectAll();
    bool serialised = false;
    const QJsonObject object = compact.toJson(&serialised);
    QVERIFY(serialised);

    bool restoredOk = false;
    QString warning;
    const QSize theoreticalMaximum(std::numeric_limits<int>::max(),
                                   std::numeric_limits<int>::max());
    const SelectionMask restored = SelectionMask::fromJson(object,
                                                            theoreticalMaximum,
                                                            &restoredOk,
                                                            &warning);
    QVERIFY2(restoredOk, qPrintable(warning));
    QVERIFY(warning.isEmpty());
    QCOMPARE(restored.size(), theoreticalMaximum);
    QVERIFY(restored.isActive());
    QVERIFY(restored.isFull());
    QCOMPARE(restored.explicitTileCount(), 0);
}

void CoreTests::smartSourceFoundationPersistsIdentityRevisionsAndRejectsCycles()
{
    SmartSourceRegistry registry;
    SmartSourceDescriptor dependency;
    dependency.name = QStringLiteral("Nested Source");
    QVERIFY(registry.insert(dependency));

    SmartSourceDescriptor root;
    root.name = QStringLiteral("Portrait Composite");
    root.dependencies = {dependency.id};
    QVERIFY(registry.insert(root));
    QVERIFY(registry.validate());

    SmartSourceDescriptor cyclic = dependency;
    cyclic.dependencies = {root.id};
    QString cycleError;
    QVERIFY(!registry.replace(cyclic, &cycleError));
    QVERIFY(!cycleError.isEmpty());
    QVERIFY(registry.validate());

    SmartSourceDescriptor forgedRevision = root;
    ++forgedRevision.revision;
    QString revisionError;
    QVERIFY(!registry.replace(forgedRevision, &revisionError));
    QVERIFY(!revisionError.isEmpty());

    bool registryJsonOk = false;
    const QJsonArray registryJson = registry.toJson(&registryJsonOk);
    QVERIFY(registryJsonOk);
    bool restoredRegistryOk = false;
    const SmartSourceRegistry restoredRegistry = SmartSourceRegistry::fromJson(
        registryJson, &restoredRegistryOk);
    QVERIFY(restoredRegistryOk);
    QVERIFY(restoredRegistry == registry);

    LayerNode smart;
    smart.type = LayerType::Smart;
    smart.name = QStringLiteral("Portrait Composite");
    smart.smartSource.sourceId = root.id;
    smart.smartSource.observedSourceRevision = root.revision;
    bool smartJsonOk = false;
    const QJsonObject smartJson = smart.toJson(&smartJsonOk);
    QVERIFY(smartJsonOk);
    bool restoredLayerOk = false;
    const LayerNode restoredLayer = LayerNode::fromJson(smartJson, &restoredLayerOk);
    QVERIFY(restoredLayerOk);
    QCOMPARE(restoredLayer.type, LayerType::Smart);
    QVERIFY(restoredLayer.smartSource == smart.smartSource);

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(48, 32);
    settings.name = QStringLiteral("Smart Foundation Test");
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY2(document.registerSmartSource(dependency, &error), qPrintable(error));
    QVERIFY2(document.registerSmartSource(root, &error), qPrintable(error));
    QVERIFY2(document.insertLayerAt(smart, {}, 0), "Smart Layer insertion failed");

    quint64 bumpedDependencyRevision = 0;
    QVERIFY(document.bumpSmartSourceRevision(dependency.id, &bumpedDependencyRevision));
    QCOMPARE(bumpedDependencyRevision, dependency.revision + 1);
    const SmartSourceDescriptor *updatedRoot = document.smartSources().find(root.id);
    QVERIFY(updatedRoot != nullptr);
    QCOMPARE(updatedRoot->revision, root.revision + 1);
    const LayerNode synchronized = document.layerById(smart.id);
    QCOMPARE(synchronized.smartSource.observedSourceRevision, updatedRoot->revision);
    QVERIFY(synchronized.revision > smart.revision);
    QVERIFY(!document.removeSmartSource(dependency.id, &error));
    QVERIFY(!document.removeSmartSource(root.id, &error));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("smart-foundation.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile projectFile(path);
    QVERIFY(projectFile.open(QIODevice::ReadOnly));
    const QJsonDocument savedJson = QJsonDocument::fromJson(projectFile.readAll());
    QVERIFY(savedJson.isObject());
    QCOMPARE(savedJson.object().value(QStringLiteral("version")).toInt(),
             PhotoDocument::ProjectFormatVersion);
    QVERIFY(savedJson.object().value(QStringLiteral("smartSources")).isArray());

    PhotoDocument reopened;
    QVERIFY2(reopened.loadProject(path, &error), qPrintable(error));
    QCOMPARE(reopened.smartSources().size(), 2);
    const SmartSourceDescriptor *reopenedRoot = reopened.smartSources().find(root.id);
    QVERIFY(reopenedRoot != nullptr);
    QCOMPARE(reopenedRoot->revision, root.revision + 1);
    const SmartSourceDescriptor *reopenedDependency = reopened.smartSources().find(
        dependency.id);
    QVERIFY(reopenedDependency != nullptr);
    QCOMPARE(reopenedDependency->revision, bumpedDependencyRevision);
    const LayerNode reopenedSmart = reopened.layerById(smart.id);
    QCOMPARE(reopenedSmart.type, LayerType::Smart);
    QCOMPARE(reopenedSmart.smartSource.sourceId, root.id);
    QCOMPARE(reopenedSmart.smartSource.observedSourceRevision, reopenedRoot->revision);

    SmartSourceDescriptor linked;
    linked.storage = SmartSourceStorage::Linked;
    QString linkedError;
    QVERIFY(!linked.isSafe(&linkedError));
    linked.linkedPath = QStringLiteral("assets/company-logo.vfxphoto");
    QVERIFY(linked.isSafe(&linkedError));
}

void CoreTests::linkedSmartLayerRoundTripsRelinksReplacesAndEmbeds()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;

    QImage red(24, 18, QImage::Format_RGBA8888);
    red.fill(QColor(220, 35, 25, 255));
    PhotoDocument source;
    source.setSourceImage(red, QStringLiteral("red.png"));
    const QString sourcePath = directory.filePath(QStringLiteral("source.vfxphoto"));
    QVERIFY2(source.saveProject(sourcePath, &error), qPrintable(error));
    const QUuid sourceIdentity = source.documentIdentity();

    QImage canvas(40, 32, QImage::Format_RGBA8888);
    canvas.fill(Qt::transparent);
    PhotoDocument owner;
    owner.setSourceImage(canvas, QStringLiteral("owner.png"));
    const QString ownerPath = directory.filePath(QStringLiteral("owner.vfxphoto"));
    QVERIFY2(owner.saveProject(ownerPath, &error), qPrintable(error));

    const QUuid linkedLayerId = owner.createLinkedSmartLayer(sourcePath, {}, &error);
    QVERIFY2(!linkedLayerId.isNull(), qPrintable(error));
    const LayerNode initialInstance = owner.layerById(linkedLayerId);
    const SmartSourceDescriptor *initialSource = owner.smartSources().find(
        initialInstance.smartSource.sourceId);
    QVERIFY(initialSource != nullptr);
    QCOMPARE(initialSource->storage, SmartSourceStorage::Linked);
    QCOMPARE(initialSource->linkedDocumentId, sourceIdentity);
    QVERIFY(initialSource->linkedAvailable);
    QVERIFY(initialSource->linkedContentFingerprint.size() == 32);
    QVERIFY(!initialSource->presentationImage.isNull());

    const QUuid filterId = owner.addLiveFilter(linkedLayerId, AdjustmentType::Invert, &error);
    QVERIFY2(!filterId.isNull(), qPrintable(error));
    const QUuid effectId = owner.addLayerEffect(linkedLayerId, LayerEffectType::DropShadow, &error);
    QVERIFY2(!effectId.isNull(), qPrintable(error));
    QVERIFY(owner.addMask(linkedLayerId));
    QVERIFY(owner.updateLayer(linkedLayerId, [](LayerNode &layer) {
        layer.opacity = 0.43;
        layer.blendMode = BlendMode::Screen;
        layer.transform.translate(5.0, 7.0);
    }));
    const LayerNode instanceBeforeSourceChange = owner.layerById(linkedLayerId);

    QVERIFY2(owner.saveProject(ownerPath, &error), qPrintable(error));
    const QString saveAsDirectory = directory.filePath(QStringLiteral("relocated-owner"));
    QVERIFY(QDir().mkpath(saveAsDirectory));
    const QString saveAsOwnerPath = QDir(saveAsDirectory).filePath(
        QStringLiteral("owner-copy.vfxphoto"));
    QVERIFY2(owner.saveProject(saveAsOwnerPath, &error), qPrintable(error));
    QCOMPARE(QFileInfo(owner.resolvedLinkedSmartSourcePath(linkedLayerId)).canonicalFilePath(),
             QFileInfo(sourcePath).canonicalFilePath());
    QVERIFY(!owner.saveProject(sourcePath, &error));
    QVERIFY2(owner.saveProject(ownerPath, &error), qPrintable(error));

    QFile ownerFile(ownerPath);
    QVERIFY(ownerFile.open(QIODevice::ReadOnly));
    const QJsonDocument ownerJson = QJsonDocument::fromJson(ownerFile.readAll());
    QVERIFY(ownerJson.isObject());
    QCOMPARE(ownerJson.object().value(QStringLiteral("version")).toInt(), 27);
    QVERIFY(!ownerJson.object().value(QStringLiteral("documentIdentity")).toString().isEmpty());
    const QJsonArray persistedSources =
        ownerJson.object().value(QStringLiteral("smartSources")).toArray();
    QVERIFY(!persistedSources.isEmpty());
    const QJsonObject persistedLinked = persistedSources.at(0).toObject();
    QCOMPARE(persistedLinked.value(QStringLiteral("schema")).toInt(), 3);
    QCOMPARE(QUuid(persistedLinked.value(QStringLiteral("linkedDocumentId")).toString()),
             sourceIdentity);
    QCOMPARE(persistedLinked.value(QStringLiteral("linkedPath")).toString(),
             QStringLiteral("source.vfxphoto"));
    QCOMPARE(persistedLinked.value(QStringLiteral("linkedFingerprint")).toString().size(), 64);
    QVERIFY(!persistedLinked.contains(QStringLiteral("linkedResolvedDocumentIds")));

    PhotoDocument reopened;
    QVERIFY2(reopened.loadProject(ownerPath, &error), qPrintable(error));
    const LayerNode reopenedInstance = reopened.layerById(linkedLayerId);
    const SmartSourceDescriptor *reopenedSource = reopened.smartSources().find(
        reopenedInstance.smartSource.sourceId);
    QVERIFY(reopenedSource != nullptr);
    QCOMPARE(reopenedSource->storage, SmartSourceStorage::Linked);
    QCOMPARE(reopenedSource->linkedDocumentId, sourceIdentity);
    QVERIFY(reopenedSource->linkedAvailable);
    QCOMPARE(reopenedInstance.opacity, instanceBeforeSourceChange.opacity);
    QCOMPARE(reopenedInstance.blendMode, instanceBeforeSourceChange.blendMode);
    QVERIFY(transformsClose(reopenedInstance.transform, instanceBeforeSourceChange.transform));
    QCOMPARE(reopenedInstance.liveFilters.size(), 1);
    QCOMPARE(reopenedInstance.layerEffects.size(), 1);
    QVERIFY(reopenedInstance.hasMask());

    const quint64 unchangedRevision = reopenedSource->revision;
    QHash<QUuid, quint64> unchangedChanges;
    QStringList unchangedWarnings;
    QVERIFY2(reopened.refreshLinkedSmartSources(&unchangedChanges, &unchangedWarnings, &error),
             qPrintable(error));
    QVERIFY(unchangedChanges.isEmpty());
    QVERIFY(unchangedWarnings.isEmpty());
    QCOMPARE(reopened.smartSources().find(reopenedInstance.smartSource.sourceId)->revision,
             unchangedRevision);

    const QString movedSourcePath = directory.filePath(QStringLiteral("moved-source.vfxphoto"));
    QVERIFY(QFile::rename(sourcePath, movedSourcePath));
    QHash<QUuid, quint64> missingChanges;
    QStringList missingWarnings;
    QVERIFY2(reopened.refreshLinkedSmartSources(&missingChanges, &missingWarnings, &error),
             qPrintable(error));
    QVERIFY(missingChanges.isEmpty());
    QVERIFY(!missingWarnings.isEmpty());
    const SmartSourceDescriptor *missingSource = reopened.smartSources().find(
        reopenedInstance.smartSource.sourceId);
    QVERIFY(missingSource != nullptr);
    QVERIFY(!missingSource->linkedAvailable);
    QCOMPARE(missingSource->linkedDocumentId, sourceIdentity);
    QVERIFY(!missingSource->presentationImage.isNull());
    const QVector<LayerNode> missingLayers = reopened.layers();
    const SmartSourceRegistry missingRegistry = reopened.smartSources();

    QVERIFY2(reopened.relinkLinkedSmartSource(linkedLayerId, movedSourcePath, &error),
             qPrintable(error));
    const SmartSourceDescriptor *relinked = reopened.smartSources().find(
        reopened.layerById(linkedLayerId).smartSource.sourceId);
    QVERIFY(relinked != nullptr);
    QVERIFY(relinked->linkedAvailable);
    QCOMPARE(relinked->linkedDocumentId, sourceIdentity);
    QVERIFY(reopened.linkedSourceWarnings().isEmpty());

    // Structural Undo/Redo restores the Smart registry, not only the layer
    // tree. Runtime missing-link diagnostics must be rebuilt from that restored
    // registry so the document cannot claim a broken link is healthy after Undo.
    QVERIFY2(reopened.replaceStructuralState(
                 reopened.sourceImage(), missingLayers, missingRegistry,
                 reopened.selectionMask().snapshot(), reopened.horizontalGuides(),
                 reopened.verticalGuides(), reopened.resolutionX(), reopened.resolutionY(),
                 reopened.colourState(), &error), qPrintable(error));
    QVERIFY(!reopened.linkedSourceWarnings().isEmpty());
    const SmartSourceDescriptor *restoredMissing = reopened.smartSources().find(
        reopened.layerById(linkedLayerId).smartSource.sourceId);
    QVERIFY(restoredMissing != nullptr);
    QVERIFY(!restoredMissing->linkedAvailable);

    // Continue the workflow from a healthy relinked state.
    QVERIFY2(reopened.relinkLinkedSmartSource(linkedLayerId, movedSourcePath, &error),
             qPrintable(error));
    QVERIFY(reopened.linkedSourceWarnings().isEmpty());

    QImage blue(24, 18, QImage::Format_RGBA8888);
    blue.fill(QColor(20, 80, 230, 255));
    PhotoDocument replacement;
    replacement.setSourceImage(blue, QStringLiteral("blue.png"));
    const QString replacementPath = directory.filePath(QStringLiteral("replacement.vfxphoto"));
    QVERIFY2(replacement.saveProject(replacementPath, &error), qPrintable(error));
    QVERIFY(replacement.documentIdentity() != sourceIdentity);
    QVERIFY(!reopened.relinkLinkedSmartSource(linkedLayerId, replacementPath, &error));
    QVERIFY2(reopened.replaceLinkedSmartSource(linkedLayerId, replacementPath, &error),
             qPrintable(error));
    const LayerNode afterReplace = reopened.layerById(linkedLayerId);
    const SmartSourceDescriptor *replaced = reopened.smartSources().find(
        afterReplace.smartSource.sourceId);
    QVERIFY(replaced != nullptr);
    QCOMPARE(replaced->linkedDocumentId, replacement.documentIdentity());
    QCOMPARE(afterReplace.opacity, instanceBeforeSourceChange.opacity);
    QCOMPARE(afterReplace.blendMode, instanceBeforeSourceChange.blendMode);
    QVERIFY(transformsClose(afterReplace.transform, instanceBeforeSourceChange.transform));
    QCOMPARE(afterReplace.liveFilters.size(), 1);
    QCOMPARE(afterReplace.layerEffects.size(), 1);
    QVERIFY(afterReplace.hasMask());

    QVERIFY2(reopened.embedLinkedSmartSource(linkedLayerId, &error), qPrintable(error));
    const SmartSourceDescriptor *embedded = reopened.smartSources().find(
        reopened.layerById(linkedLayerId).smartSource.sourceId);
    QVERIFY(embedded != nullptr);
    QCOMPARE(embedded->storage, SmartSourceStorage::Embedded);
    QVERIFY(embedded->hasEmbeddedDocument());
    QVERIFY(embedded->linkedPath.isEmpty());
    QVERIFY(QFile::remove(replacementPath));
    QHash<QUuid, quint64> postEmbedChanges;
    QStringList postEmbedWarnings;
    QVERIFY2(reopened.refreshLinkedSmartSources(&postEmbedChanges, &postEmbedWarnings, &error),
             qPrintable(error));
    QVERIFY(postEmbedWarnings.isEmpty());

    // A project created before 0.14.0l has no persisted document UUID yet. Its
    // migration identity must still survive a pure filesystem move so Relink
    // can recognise the same legacy source before the source is first upgraded.
    PhotoDocument legacySource;
    legacySource.setSourceImage(red, QStringLiteral("legacy.png"));
    const QString legacyPath = directory.filePath(QStringLiteral("legacy-source.vfxphoto"));
    QVERIFY2(legacySource.saveProject(legacyPath, &error), qPrintable(error));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::ReadOnly));
    QJsonDocument legacyJson = QJsonDocument::fromJson(legacyFile.readAll());
    legacyFile.close();
    QVERIFY(legacyJson.isObject());
    QJsonObject legacyRoot = legacyJson.object();
    legacyRoot.insert(QStringLiteral("version"), 26);
    legacyRoot.remove(QStringLiteral("documentIdentity"));
    legacyJson.setObject(legacyRoot);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray legacyBytes = legacyJson.toJson(QJsonDocument::Compact);
    QCOMPARE(legacyFile.write(legacyBytes), static_cast<qint64>(legacyBytes.size()));
    legacyFile.close();

    PhotoDocument legacyOwner;
    legacyOwner.setSourceImage(canvas, QStringLiteral("legacy-owner.png"));
    const QString legacyOwnerPath = directory.filePath(QStringLiteral("legacy-owner.vfxphoto"));
    QVERIFY2(legacyOwner.saveProject(legacyOwnerPath, &error), qPrintable(error));
    const QUuid legacyLinkedLayer = legacyOwner.createLinkedSmartLayer(legacyPath, {}, &error);
    QVERIFY2(!legacyLinkedLayer.isNull(), qPrintable(error));
    const QString movedLegacyPath = directory.filePath(QStringLiteral("legacy-source-moved.vfxphoto"));
    QVERIFY(QFile::rename(legacyPath, movedLegacyPath));
    QVERIFY2(legacyOwner.relinkLinkedSmartSource(legacyLinkedLayer, movedLegacyPath, &error),
             qPrintable(error));
}

void CoreTests::linkedSmartLayerPropagatesNestedRevisionsAndRejectsFileCycles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;

    auto makeDocument = [](const QColor &colour) {
        QImage image(20, 16, QImage::Format_RGBA8888);
        image.fill(colour);
        PhotoDocument document;
        document.setSourceImage(image, QString());
        return document;
    };

    PhotoDocument c = makeDocument(QColor(180, 30, 30, 255));
    const QString cPath = directory.filePath(QStringLiteral("C.vfxphoto"));
    QVERIFY2(c.saveProject(cPath, &error), qPrintable(error));

    PhotoDocument b = makeDocument(QColor(0, 0, 0, 0));
    const QString bPath = directory.filePath(QStringLiteral("B.vfxphoto"));
    QVERIFY2(b.saveProject(bPath, &error), qPrintable(error));
    const QUuid bLink = b.createLinkedSmartLayer(cPath, {}, &error);
    QVERIFY2(!bLink.isNull(), qPrintable(error));
    QVERIFY2(b.saveProject(bPath, &error), qPrintable(error));

    PhotoDocument a = makeDocument(QColor(0, 0, 0, 0));
    const QString aPath = directory.filePath(QStringLiteral("A.vfxphoto"));
    QVERIFY2(a.saveProject(aPath, &error), qPrintable(error));
    const QUuid aLink = a.createLinkedSmartLayer(bPath, {}, &error);
    QVERIFY2(!aLink.isNull(), qPrintable(error));
    QVERIFY2(a.saveProject(aPath, &error), qPrintable(error));
    const SmartSourceDescriptor *aBefore = a.smartSources().find(
        a.layerById(aLink).smartSource.sourceId);
    QVERIFY(aBefore != nullptr);
    const QByteArray nestedFingerprintBefore = aBefore->linkedContentFingerprint;
    QVERIFY(aBefore->linkedResolvedDocumentIds.contains(b.documentIdentity()));
    QVERIFY(aBefore->linkedResolvedDocumentIds.contains(c.documentIdentity()));
    QVERIFY(a.dependsOnLinkedDocument(c.documentIdentity(), cPath));

    PhotoDocument unrelated = makeDocument(QColor(30, 40, 200, 255));
    const QString unrelatedPath = directory.filePath(QStringLiteral("Unrelated.vfxphoto"));
    QVERIFY2(unrelated.saveProject(unrelatedPath, &error), qPrintable(error));
    QVERIFY(!a.dependsOnLinkedDocument(unrelated.documentIdentity(), unrelatedPath));

    const QUuid cBase = c.baseLayerId();
    QImage changedPixels(20, 16, QImage::Format_RGBA8888);
    changedPixels.fill(QColor(25, 210, 80, 255));
    QVERIFY(c.updateLayer(cBase, [&](LayerNode &layer) { layer.rasterImage = changedPixels; }));
    QVERIFY2(c.saveProject(cPath, &error), qPrintable(error));

    QHash<QUuid, quint64> changed;
    QStringList warnings;
    QVERIFY2(a.refreshLinkedSmartSources(&changed, &warnings, &error,
                                         c.documentIdentity(), cPath),
             qPrintable(error));
    QVERIFY(warnings.isEmpty());
    QVERIFY(!changed.isEmpty());
    const SmartSourceDescriptor *aAfter = a.smartSources().find(
        a.layerById(aLink).smartSource.sourceId);
    QVERIFY(aAfter != nullptr);
    QVERIFY(aAfter->linkedContentFingerprint != nestedFingerprintBefore);

    // A targeted notification for an unrelated open document must not reopen or
    // advance this linked source branch.
    const quint64 relevantRevision = aAfter->revision;
    const QByteArray relevantFingerprint = aAfter->linkedContentFingerprint;
    QHash<QUuid, quint64> unrelatedChanges;
    QStringList unrelatedWarnings;
    QVERIFY2(a.refreshLinkedSmartSources(&unrelatedChanges, &unrelatedWarnings, &error,
                                         unrelated.documentIdentity(), unrelatedPath),
             qPrintable(error));
    QVERIFY(unrelatedChanges.isEmpty());
    QVERIFY(unrelatedWarnings.isEmpty());
    const SmartSourceDescriptor *afterUnrelated = a.smartSources().find(
        a.layerById(aLink).smartSource.sourceId);
    QVERIFY(afterUnrelated != nullptr);
    QCOMPARE(afterUnrelated->revision, relevantRevision);
    QCOMPARE(afterUnrelated->linkedContentFingerprint, relevantFingerprint);

    // B -> C already exists. Linking C -> A would create C -> A -> B -> C
    // and must be rejected before any source graph is committed.
    const QUuid circular = c.createLinkedSmartLayer(aPath, {}, &error);
    QVERIFY(circular.isNull());
    QVERIFY2(error.contains(QStringLiteral("Circular linked Smart Layer dependency")),
             qPrintable(error));
}

void CoreTests::smartLiveFilterFxIntegratedRoundTripPreservesAppearance()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("0.14.0m Integrated Smart Workflow");
    settings.pixelSize = QSize(72, 56);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid rasterId = document.addRasterLayer();
    QVERIFY(!rasterId.isNull());
    QImage pixels(settings.pixelSize, QImage::Format_RGBA64);
    pixels.fill(Qt::transparent);
    pixels.setColorSpace(document.sourceImage().colorSpace());
    QPainter painter(&pixels);
    painter.fillRect(QRect(11, 9, 39, 31), QColor(210, 55, 35, 220));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(35, 170, 230, 155));
    painter.drawEllipse(QRect(29, 19, 31, 28));
    painter.end();
    pixels.setPixelColor(64, 7, QColor(180, 40, 210, 0));
    QVERIFY(document.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = settings.pixelSize;
        layer.rasterReferenceOrigin = {};
    }));

    const QUuid smartId = document.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    QVERIFY(document.updateLayer(smartId, [](LayerNode &layer) {
        layer.transform.translate(4.0, 3.0);
        layer.transform.rotate(7.0);
        layer.opacity = 0.82;
        layer.blendMode = BlendMode::Screen;
    }));

    const QUuid blurId = document.addLiveFilter(
        smartId, AdjustmentType::GaussianBlur, &error);
    QVERIFY2(!blurId.isNull(), qPrintable(error));
    AdjustmentData blur;
    blur.reset(AdjustmentType::GaussianBlur);
    auto blurParameters = std::get<GaussianBlurParameters>(blur.parameters);
    blurParameters.radius = 2.5;
    blurParameters.affectAlpha = true;
    blur.parameters = blurParameters;
    QVERIFY(document.updateLiveFilter(smartId, blurId, blur));

    const QUuid invertId = document.addLiveFilter(smartId, AdjustmentType::Invert, &error);
    QVERIFY2(!invertId.isNull(), qPrintable(error));
    QVERIFY(document.addLiveFilterMask(smartId, invertId));
    QImage filterMask(settings.pixelSize, QImage::Format_Grayscale8);
    filterMask.fill(0);
    QPainter filterPainter(&filterMask);
    filterPainter.fillRect(QRect(0, 0, settings.pixelSize.width() / 2,
                                 settings.pixelSize.height()), Qt::white);
    filterPainter.end();
    QVERIFY(document.updateLiveFilterMask(smartId, invertId, filterMask,
                                          settings.pixelSize, QPointF()));

    const QUuid shadowId = document.addLayerEffect(
        smartId, LayerEffectType::DropShadow, &error);
    QVERIFY2(!shadowId.isNull(), qPrintable(error));
    LayerNode configuredSmart = document.layerById(smartId);
    auto shadowIt = std::find_if(configuredSmart.layerEffects.begin(),
                                 configuredSmart.layerEffects.end(),
                                 [shadowId](const LayerEffect &effect) {
                                     return effect.id == shadowId;
                                 });
    QVERIFY(shadowIt != configuredSmart.layerEffects.end());
    LayerEffect shadow = *shadowIt;
    shadow.distance = 5.0;
    shadow.size = 4.0;
    shadow.spread = 15.0;
    shadow.effectOpacity = 0.60;
    QVERIFY2(document.updateLayerEffect(smartId, shadowId, shadow, &error),
             qPrintable(error));

    const QUuid bevelId = document.addLayerEffect(
        smartId, LayerEffectType::BevelEmboss, &error);
    QVERIFY2(!bevelId.isNull(), qPrintable(error));
    configuredSmart = document.layerById(smartId);
    auto bevelIt = std::find_if(configuredSmart.layerEffects.begin(),
                                configuredSmart.layerEffects.end(),
                                [bevelId](const LayerEffect &effect) {
                                    return effect.id == bevelId;
                                });
    QVERIFY(bevelIt != configuredSmart.layerEffects.end());
    LayerEffect bevel = *bevelIt;
    bevel.size = 3.0;
    bevel.bevelDepth = 145.0;
    bevel.bevelSoften = 0.75;
    QVERIFY2(document.updateLayerEffect(smartId, bevelId, bevel, &error),
             qPrintable(error));

    QVERIFY(document.addMask(smartId));
    QImage instanceMask(settings.pixelSize, QImage::Format_Grayscale8);
    instanceMask.fill(255);
    QPainter maskPainter(&instanceMask);
    maskPainter.fillRect(QRect(0, 0, 13, settings.pixelSize.height()), Qt::black);
    maskPainter.end();
    QVERIFY(document.updateLayer(smartId, [&](LayerNode &layer) {
        layer.maskImage = instanceMask;
        layer.maskReferenceSize = settings.pixelSize;
        layer.maskReferenceOrigin = {};
        layer.maskEnabled = true;
        layer.maskInverted = false;
    }));

    const QImage before = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!before.isNull());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("integrated-smart-workflow.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument reopened;
    QVERIFY2(reopened.loadProject(path, &error), qPrintable(error));
    const LayerNode reopenedSmart = reopened.layerById(smartId);
    QCOMPARE(reopenedSmart.liveFilters.size(), 2);
    QCOMPARE(reopenedSmart.layerEffects.size(), 2);
    QVERIFY(reopenedSmart.hasMask());
    QCOMPARE(reopenedSmart.opacity, 0.82);
    QCOMPARE(reopenedSmart.blendMode, BlendMode::Screen);
    QVERIFY(transformsClose(reopenedSmart.transform, document.layerById(smartId).transform));

    const QImage after = ImageProcessor::renderPreservingHiddenRgb(
        reopened.sourceImage(), reopened.layers(), nullptr, reopened.sourceImage().size(),
        reopened.colourState().processingCompatibility);
    QVERIFY(!after.isNull());
    QVERIFY(imagesWithinChannelTolerance(before, after, 1));

    // Hidden RGB in the authoritative embedded source is still data even when
    // alpha is zero; the integrated filter/fx presentation must not destructively
    // rewrite the source payload while round-tripping the project.
    const SmartSourceDescriptor *source = reopened.smartSources().find(
        reopenedSmart.smartSource.sourceId);
    QVERIFY(source != nullptr);
    QVERIFY(source->hasEmbeddedDocument());
    QVector<LayerNode> embeddedLayers;
    QVERIFY2(reopened.embeddedSmartSourceLayers(
                 source->id, &embeddedLayers, nullptr, nullptr, &error),
             qPrintable(error));
    QCOMPARE(embeddedLayers.size(), 1);
    const QImage embeddedPixels = embeddedLayers.constFirst().rasterImage
        .convertToFormat(QImage::Format_RGBA64);
    const QRgba64 hidden = reinterpret_cast<const QRgba64 *>(
        embeddedPixels.constScanLine(7))[64];
    QCOMPARE(hidden.alpha(), static_cast<quint16>(0));
    QVERIFY(hidden.red() > 0 || hidden.green() > 0 || hidden.blue() > 0);
}

void CoreTests::smartLayerEmbeddedConversionPreservesStructureAppearanceAndPersistence()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Embedded Smart Conversion");
    settings.pixelSize = QSize(72, 54);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const QUuid baseId = document.baseLayerId();
    QVERIFY(document.updateLayer(baseId, [](LayerNode &layer) { layer.visible = false; }));
    const QUuid lowerId = document.addRasterLayer(baseId);
    QVERIFY(!lowerId.isNull());
    QImage lowerPixels(settings.pixelSize, QImage::Format_RGBA8888);
    lowerPixels.fill(Qt::transparent);
    for (int y = 8; y < 42; ++y) {
        uchar *row = lowerPixels.scanLine(y);
        for (int x = 9; x < 49; ++x) {
            const int offset = x * 4;
            row[offset] = static_cast<uchar>(40 + (x * 3) % 160);
            row[offset + 1] = static_cast<uchar>(20 + (y * 5) % 180);
            row[offset + 2] = 190;
            row[offset + 3] = static_cast<uchar>(80 + ((x + y) % 176));
        }
    }
    // Authoritative hidden RGB must survive both the embedded source and the
    // derived parent-facing Smart presentation.
    lowerPixels.scanLine(2)[4] = 211;
    lowerPixels.scanLine(2)[5] = 77;
    lowerPixels.scanLine(2)[6] = 143;
    lowerPixels.scanLine(2)[7] = 0;
    QVERIFY(document.updateLayer(lowerId, [&](LayerNode &layer) {
        layer.name = QStringLiteral("Retouch Pixels");
        layer.rasterImage = lowerPixels;
        layer.opacity = 0.82;
    }));

    const QUuid groupId = document.groupLayers({lowerId}, QStringLiteral("Retouch Group"));
    QVERIFY(!groupId.isNull());
    const QUuid exposureId = document.addAdjustment(AdjustmentType::Exposure, groupId);
    QVERIFY(!exposureId.isNull());
    QVERIFY(document.updateLayer(exposureId, [](LayerNode &layer) {
        layer.exposure = 0.35;
    }));

    const QUuid upperId = document.addRasterLayer();
    QVERIFY(!upperId.isNull());
    QImage upperPixels(settings.pixelSize, QImage::Format_RGBA8888);
    upperPixels.fill(Qt::transparent);
    QPainter painter(&upperPixels);
    painter.fillRect(QRect(31, 13, 29, 31), QColor(35, 180, 95, 170));
    painter.end();
    QVERIFY(document.updateLayer(upperId, [&](LayerNode &layer) {
        layer.name = QStringLiteral("Logo");
        layer.rasterImage = upperPixels;
        layer.opacity = 0.7;
        layer.transform.translate(3.0, -2.0);
    }));

    const QVector<LayerNode> beforeLayers = document.layers();
    const SmartSourceRegistry beforeSources = document.smartSources();
    const SelectionMask::Snapshot beforeSelection = document.selectionMask().snapshot();
    const DocumentColourState beforeColour = document.colourState();
    const QImage beforeRender = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!beforeRender.isNull());

    QVERIFY2(document.canConvertLayersToSmart({upperId, groupId}, &error), qPrintable(error));
    const QUuid smartId = document.convertLayersToEmbeddedSmart({upperId, groupId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    QCOMPARE(document.smartSources().size(), 1);
    const LayerNode smart = document.layerById(smartId);
    QCOMPARE(smart.type, LayerType::Smart);
    QVERIFY(!smart.smartPresentationImage.isNull());

    const SmartSourceDescriptor *source = document.smartSources().find(smart.smartSource.sourceId);
    QVERIFY(source != nullptr);
    QVERIFY(source->hasEmbeddedDocument());
    QVERIFY(source->hasCurrentPresentation());
    QCOMPARE(source->storage, SmartSourceStorage::Embedded);

    QVector<LayerNode> embedded;
    QSize embeddedCanvas;
    DocumentColourState embeddedColour;
    QVERIFY2(document.embeddedSmartSourceLayers(source->id, &embedded, &embeddedCanvas,
                                                &embeddedColour, &error), qPrintable(error));
    QCOMPARE(embeddedCanvas, settings.pixelSize);
    QCOMPARE(embedded.size(), 2);
    QCOMPARE(embedded.at(0).id, upperId);
    QCOMPARE(embedded.at(1).id, groupId);
    QCOMPARE(embedded.at(1).children.size(), 2);
    QVERIFY(embeddedColour.semanticallyEquals(document.colourState()));
    const LayerNode embeddedLower = embedded.at(1).children.at(1);
    QCOMPARE(embeddedLower.id, lowerId);
    const QImage embeddedLowerPixels = embeddedLower.rasterImage.convertToFormat(QImage::Format_RGBA8888);
    const uchar *hiddenRow = embeddedLowerPixels.constScanLine(2);
    QCOMPARE(hiddenRow[4], static_cast<uchar>(211));
    QCOMPARE(hiddenRow[5], static_cast<uchar>(77));
    QCOMPARE(hiddenRow[6], static_cast<uchar>(143));
    QCOMPARE(hiddenRow[7], static_cast<uchar>(0));

    const QImage afterRender = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!afterRender.isNull());
    QVERIFY(imagesWithinChannelTolerance(beforeRender, afterRender, 1));
    const QImage beforeStraight = beforeRender.convertToFormat(QImage::Format_RGBA8888);
    const QImage afterStraight = afterRender.convertToFormat(QImage::Format_RGBA8888);
    const uchar *beforeHidden = beforeStraight.constScanLine(2) + 4;
    const uchar *afterHidden = afterStraight.constScanLine(2) + 4;
    QCOMPARE(beforeHidden[3], static_cast<uchar>(0));
    QCOMPARE(afterHidden[3], static_cast<uchar>(0));
    QVERIFY(beforeHidden[0] != 0 || beforeHidden[1] != 0 || beforeHidden[2] != 0);
    QCOMPARE(afterHidden[0], beforeHidden[0]);
    QCOMPARE(afterHidden[1], beforeHidden[1]);
    QCOMPARE(afterHidden[2], beforeHidden[2]);

    // Structural state replacement is the same path used by MainWindow Undo.
    const QVector<LayerNode> convertedLayers = document.layers();
    const SmartSourceRegistry convertedSources = document.smartSources();
    QVERIFY2(document.replaceStructuralState(document.sourceImage(), beforeLayers, beforeSources,
                                             beforeSelection, document.horizontalGuides(),
                                             document.verticalGuides(), document.resolutionX(),
                                             document.resolutionY(), beforeColour, &error),
             qPrintable(error));
    QVERIFY(document.smartSources().isEmpty());
    QVERIFY(document.containsLayer(upperId));
    QVERIFY(document.containsLayer(groupId));
    QVERIFY(!document.containsLayer(smartId));

    QVERIFY2(document.replaceStructuralState(document.sourceImage(), convertedLayers,
                                             convertedSources, beforeSelection,
                                             document.horizontalGuides(), document.verticalGuides(),
                                             document.resolutionX(), document.resolutionY(),
                                             beforeColour, &error), qPrintable(error));
    QVERIFY(document.containsLayer(smartId));
    const QImage redoRender = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(imagesWithinChannelTolerance(afterRender, redoRender, 1));

    const QUuid topAdjustment = document.addAdjustment(AdjustmentType::Exposure);
    QVERIFY(!topAdjustment.isNull());
    QString backdropError;
    QVERIFY(!document.canConvertLayersToSmart({topAdjustment}, &backdropError));
    QVERIFY(!backdropError.isEmpty());
    QVERIFY(document.removeLayer(topAdjustment));

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = temporary.filePath(QStringLiteral("embedded-smart.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument reopened;
    QVERIFY2(reopened.loadProject(path, &error), qPrintable(error));
    QCOMPARE(reopened.smartSources().size(), 1);
    const LayerNode reopenedSmart = reopened.layerById(smartId);
    QCOMPARE(reopenedSmart.type, LayerType::Smart);
    QVERIFY(!reopenedSmart.smartPresentationImage.isNull());
    QVector<LayerNode> reopenedEmbedded;
    QVERIFY2(reopened.embeddedSmartSourceLayers(reopenedSmart.smartSource.sourceId,
                                                &reopenedEmbedded, nullptr, nullptr, &error),
             qPrintable(error));
    QCOMPARE(reopenedEmbedded.size(), 2);
    QCOMPARE(reopenedEmbedded.at(1).children.size(), 2);
    const QImage reopenedRender = ImageProcessor::renderPreservingHiddenRgb(
        reopened.sourceImage(), reopened.layers(), nullptr, reopened.sourceImage().size(),
        reopened.colourState().processingCompatibility);
    QVERIFY(imagesWithinChannelTolerance(afterRender, reopenedRender, 1));
}

void CoreTests::smartLayerEditContentsCommitsAndPropagatesDependencies()
{
    PhotoDocument owner;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Smart Dependency Edit");
    settings.pixelSize = QSize(40, 30);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(owner.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(owner.updateLayer(owner.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid rasterId = owner.addRasterLayer();
    QVERIFY(!rasterId.isNull());
    QImage initial(settings.pixelSize, QImage::Format_RGBA8888);
    initial.fill(Qt::transparent);
    QPainter painter(&initial);
    painter.fillRect(QRect(5, 4, 18, 13), QColor(210, 40, 70, 190));
    painter.end();
    initial.setColorSpace(owner.sourceImage().colorSpace());
    QVERIFY(owner.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.name = QStringLiteral("Nested Raster");
        layer.rasterImage = initial;
    }));

    const QUuid smartB = owner.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartB.isNull(), qPrintable(error));
    const QUuid sourceB = owner.layerById(smartB).smartSource.sourceId;
    QVERIFY(!sourceB.isNull());
    const QUuid smartA = owner.convertLayersToEmbeddedSmart({smartB}, &error);
    QVERIFY2(!smartA.isNull(), qPrintable(error));
    const QUuid sourceA = owner.layerById(smartA).smartSource.sourceId;
    QVERIFY(!sourceA.isNull());
    QVERIFY(sourceA != sourceB);

    const SmartSourceDescriptor *beforeB = owner.smartSources().find(sourceB);
    const SmartSourceDescriptor *beforeA = owner.smartSources().find(sourceA);
    QVERIFY(beforeB != nullptr);
    QVERIFY(beforeA != nullptr);
    QVERIFY(beforeA->dependencies.contains(sourceB));
    const quint64 revisionBBefore = beforeB->revision;
    const quint64 revisionABefore = beforeA->revision;
    const QImage renderedBefore = ImageProcessor::renderPreservingHiddenRgb(
        owner.sourceImage(), owner.layers(), nullptr, owner.sourceImage().size(),
        owner.colourState().processingCompatibility);
    QVERIFY(!renderedBefore.isNull());

    PhotoDocument editableB;
    QHash<QUuid, quint64> baseline;
    QVERIFY2(owner.createEditableSmartSourceDocument(sourceB, &editableB,
                                                       &baseline, &error),
             qPrintable(error));
    QVERIFY(baseline.contains(sourceA));
    QVERIFY(baseline.contains(sourceB));
    QImage changed = initial;
    changed.detach();
    QPainter changedPainter(&changed);
    changedPainter.fillRect(QRect(10, 9, 20, 15), QColor(25, 205, 130, 230));
    changedPainter.end();
    QVERIFY(editableB.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = changed;
    }));

    QHash<QUuid, quint64> changedRevisions;
    QVERIFY2(owner.commitEditableSmartSourceDocument(sourceB, editableB, baseline,
                                                       &changedRevisions, &error),
             qPrintable(error));
    QVERIFY(changedRevisions.contains(sourceB));
    QVERIFY(changedRevisions.contains(sourceA));
    const SmartSourceDescriptor *afterB = owner.smartSources().find(sourceB);
    const SmartSourceDescriptor *afterA = owner.smartSources().find(sourceA);
    QVERIFY(afterB != nullptr);
    QVERIFY(afterA != nullptr);
    QCOMPARE(afterB->revision, revisionBBefore + 1);
    QCOMPARE(afterA->revision, revisionABefore + 1);
    QCOMPARE(afterB->presentationRevision, afterB->revision);
    QCOMPARE(afterA->presentationRevision, afterA->revision);
    QCOMPARE(owner.layerById(smartA).smartSource.observedSourceRevision,
             afterA->revision);
    QCOMPARE(afterB->embeddedDocument.value(QStringLiteral("schema")).toInt(), 10);
    QCOMPARE(afterB->embeddedDocument.value(QStringLiteral("bitDepth")).toInt(), 8);

    QVector<LayerNode> sourceALayers;
    QVERIFY2(owner.embeddedSmartSourceLayers(sourceA, &sourceALayers,
                                              nullptr, nullptr, &error),
             qPrintable(error));
    QCOMPARE(sourceALayers.size(), 1);
    QCOMPARE(sourceALayers.constFirst().type, LayerType::Smart);
    QCOMPARE(sourceALayers.constFirst().smartSource.sourceId, sourceB);
    QCOMPARE(sourceALayers.constFirst().smartSource.observedSourceRevision,
             afterB->revision);

    const QImage renderedAfter = ImageProcessor::renderPreservingHiddenRgb(
        owner.sourceImage(), owner.layers(), nullptr, owner.sourceImage().size(),
        owner.colourState().processingCompatibility);
    QVERIFY(!renderedAfter.isNull());
    QVERIFY(!exactImagesEqual(renderedBefore, renderedAfter));

    QTemporaryDir migrationDir;
    QVERIFY(migrationDir.isValid());
    const QString currentPath = migrationDir.filePath(QStringLiteral("smart-v20.vfxphoto"));
    QVERIFY2(owner.saveProject(currentPath, &error), qPrintable(error));
    QFile currentFile(currentPath);
    QVERIFY(currentFile.open(QIODevice::ReadOnly));
    const QJsonDocument currentJson = QJsonDocument::fromJson(currentFile.readAll());
    QVERIFY(currentJson.isObject());
    currentFile.close();

    QJsonObject legacyRoot = currentJson.object();
    legacyRoot.insert(QStringLiteral("version"), 18);
    QJsonArray legacyTree = legacyRoot.value(QStringLiteral("layerTree")).toArray();
    stripSmartTransformMetadata(&legacyTree);
    legacyRoot.insert(QStringLiteral("layerTree"), legacyTree);
    QJsonArray legacySources = legacyRoot.value(QStringLiteral("smartSources")).toArray();
    for (qsizetype i = 0; i < legacySources.size(); ++i) {
        QJsonObject descriptor = legacySources.at(i).toObject();
        QJsonObject embedded = descriptor.value(QStringLiteral("embeddedDocument")).toObject();
        if (!embedded.isEmpty()) {
            embedded.insert(QStringLiteral("schema"), 1);
            embedded.remove(QStringLiteral("bitDepth"));
            QJsonArray embeddedLayers = embedded.value(QStringLiteral("layers")).toArray();
            stripSmartTransformMetadata(&embeddedLayers);
            embedded.insert(QStringLiteral("layers"), embeddedLayers);
            descriptor.insert(QStringLiteral("embeddedDocument"), embedded);
            legacySources.replace(i, descriptor);
        }
    }
    legacyRoot.insert(QStringLiteral("smartSources"), legacySources);
    const QString legacyPath = migrationDir.filePath(QStringLiteral("smart-v18-schema1.vfxphoto"));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    QVERIFY(legacyFile.write(QJsonDocument(legacyRoot).toJson(QJsonDocument::Compact)) > 0);
    legacyFile.close();
    PhotoDocument migrated;
    QVERIFY2(migrated.loadProject(legacyPath, &error), qPrintable(error));

    QJsonObject forgedRoot = currentJson.object();
    forgedRoot.insert(QStringLiteral("version"), 18);
    QJsonArray forgedTree = forgedRoot.value(QStringLiteral("layerTree")).toArray();
    stripSmartTransformMetadata(&forgedTree);
    forgedRoot.insert(QStringLiteral("layerTree"), forgedTree);
    QJsonArray forgedSources = forgedRoot.value(QStringLiteral("smartSources")).toArray();
    for (qsizetype i = 0; i < forgedSources.size(); ++i) {
        QJsonObject descriptor = forgedSources.at(i).toObject();
        QJsonObject embedded = descriptor.value(QStringLiteral("embeddedDocument")).toObject();
        if (!embedded.isEmpty()) {
            embedded.insert(QStringLiteral("schema"), 2);
            QJsonArray embeddedLayers = embedded.value(QStringLiteral("layers")).toArray();
            stripSmartTransformMetadata(&embeddedLayers);
            embedded.insert(QStringLiteral("layers"), embeddedLayers);
            descriptor.insert(QStringLiteral("embeddedDocument"), embedded);
            forgedSources.replace(i, descriptor);
        }
    }
    forgedRoot.insert(QStringLiteral("smartSources"), forgedSources);
    const QString forgedPath = migrationDir.filePath(QStringLiteral("smart-v18-forged-schema2.vfxphoto"));
    QFile forgedFile(forgedPath);
    QVERIFY(forgedFile.open(QIODevice::WriteOnly));
    QVERIFY(forgedFile.write(QJsonDocument(forgedRoot).toJson(QJsonDocument::Compact)) > 0);
    forgedFile.close();
    PhotoDocument forged;
    QString forgedError;
    QVERIFY(!forged.loadProject(forgedPath, &forgedError));
    QVERIFY(forgedError.contains(QStringLiteral("precision"), Qt::CaseInsensitive));

    // Embedded schema 2 predates persistent nested Smart transform sampling.
    // Keeping the schema old while retaining a nested SmartTransform payload
    // must be rejected rather than silently changing the old format contract.
    QJsonObject forgedTransformRoot = currentJson.object();
    QJsonArray forgedTransformSources = forgedTransformRoot
        .value(QStringLiteral("smartSources")).toArray();
    for (qsizetype i = 0; i < forgedTransformSources.size(); ++i) {
        QJsonObject descriptor = forgedTransformSources.at(i).toObject();
        QJsonObject embedded = descriptor.value(QStringLiteral("embeddedDocument")).toObject();
        if (!embedded.isEmpty()) {
            embedded.insert(QStringLiteral("schema"), 2);
            descriptor.insert(QStringLiteral("embeddedDocument"), embedded);
            forgedTransformSources.replace(i, descriptor);
        }
    }
    forgedTransformRoot.insert(QStringLiteral("smartSources"), forgedTransformSources);
    const QString forgedTransformPath = migrationDir.filePath(
        QStringLiteral("smart-v20-forged-schema2-transform.vfxphoto"));
    QFile forgedTransformFile(forgedTransformPath);
    QVERIFY(forgedTransformFile.open(QIODevice::WriteOnly));
    QVERIFY(forgedTransformFile.write(
                QJsonDocument(forgedTransformRoot).toJson(QJsonDocument::Compact)) > 0);
    forgedTransformFile.close();
    PhotoDocument forgedTransform;
    QString forgedTransformError;
    QVERIFY(!forgedTransform.loadProject(forgedTransformPath, &forgedTransformError));
    QVERIFY(forgedTransformError.contains(QStringLiteral("schema-3"),
                                          Qt::CaseInsensitive)
            || forgedTransformError.contains(QStringLiteral("transform"),
                                             Qt::CaseInsensitive));

    const SmartSourceRegistry registryBeforeCycle = owner.smartSources();
    const quint64 rootObservedBeforeCycle =
        owner.layerById(smartA).smartSource.observedSourceRevision;
    PhotoDocument cycleEditor;
    QHash<QUuid, quint64> cycleBaseline;
    QVERIFY2(owner.createEditableSmartSourceDocument(sourceB, &cycleEditor,
                                                       &cycleBaseline, &error),
             qPrintable(error));
    LayerNode cycleReference;
    cycleReference.type = LayerType::Smart;
    cycleReference.name = QStringLiteral("Cycle Back To A");
    cycleReference.smartSource.sourceId = sourceA;
    cycleReference.smartSource.observedSourceRevision =
        owner.smartSources().find(sourceA)->revision;
    QVERIFY(cycleEditor.insertLayerAt(cycleReference, {}, 0));
    QString cycleError;
    QVERIFY(!owner.commitEditableSmartSourceDocument(sourceB, cycleEditor,
                                                       cycleBaseline, nullptr,
                                                       &cycleError));
    QVERIFY(cycleError.contains(QStringLiteral("cycle"), Qt::CaseInsensitive)
            || cycleError.contains(QStringLiteral("dependency"), Qt::CaseInsensitive));
    QVERIFY(owner.smartSources() == registryBeforeCycle);
    QCOMPARE(owner.layerById(smartA).smartSource.observedSourceRevision,
             rootObservedBeforeCycle);
}

void CoreTests::smartLayerEditContentsPreservesColourManagedComposition()
{
    PhotoDocument owner;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Smart Colour Boundary");
    settings.pixelSize = QSize(32, 24);
    settings.bitDepth = 16;
    settings.colourSpace = QColorSpace(QColorSpace::SRgb);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(owner.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(owner.updateLayer(owner.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid rasterId = owner.addRasterLayer();
    QVERIFY(!rasterId.isNull());
    QImage pixels(settings.pixelSize, QImage::Format_RGBA64);
    pixels.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < pixels.height(); ++y) {
        QRgba64 *row = reinterpret_cast<QRgba64 *>(pixels.scanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(3000 + x * 1500),
                static_cast<quint16>(5000 + y * 1800),
                static_cast<quint16>(42000 - x * 700),
                static_cast<quint16>(12000 + ((x + y) % 20) * 2200));
        }
    }
    QVERIFY(owner.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
    }));
    const QUuid smartId = owner.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    const QUuid sourceId = owner.layerById(smartId).smartSource.sourceId;
    const QImage before = ImageProcessor::renderPreservingHiddenRgb(
        owner.sourceImage(), owner.layers(), nullptr, owner.sourceImage().size(),
        owner.colourState().processingCompatibility);
    QVERIFY(!before.isNull());

    PhotoDocument editor;
    QHash<QUuid, quint64> baseline;
    QVERIFY2(owner.createEditableSmartSourceDocument(sourceId, &editor,
                                                       &baseline, &error),
             qPrintable(error));
    QCOMPARE(editor.sourceImage().colorSpace(), QColorSpace(QColorSpace::SRgb));
    PreparedColourProfileResult converted;
    const ColourSpaceDescriptor displayP3 =
        ColourSpaceDescriptor::fromQColorSpace(QColorSpace(QColorSpace::DisplayP3));
    QVERIFY2(prepareConvertedDocumentProfile(editor, displayP3, &converted,
                                              nullptr, &error),
             qPrintable(error));
    QVERIFY2(editor.replaceStructuralState(
                 converted.canvasImage, converted.layers, editor.smartSources(),
                 editor.selectionMask().snapshot(), editor.horizontalGuides(),
                 editor.verticalGuides(), editor.resolutionX(), editor.resolutionY(),
                 converted.colourState, &error),
             qPrintable(error));
    QCOMPARE(editor.sourceImage().colorSpace(), QColorSpace(QColorSpace::DisplayP3));

    QVERIFY2(owner.commitEditableSmartSourceDocument(sourceId, editor, baseline,
                                                       nullptr, &error),
             qPrintable(error));
    const SmartSourceDescriptor *source = owner.smartSources().find(sourceId);
    QVERIFY(source != nullptr);
    QCOMPARE(source->presentationImage.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
    const LayerNode rootSmart = owner.layerById(smartId);
    QCOMPARE(rootSmart.smartPresentationImage.colorSpace(), QColorSpace(QColorSpace::SRgb));
    const QImage after = ImageProcessor::renderPreservingHiddenRgb(
        owner.sourceImage(), owner.layers(), nullptr, owner.sourceImage().size(),
        owner.colourState().processingCompatibility);
    QVERIFY(!after.isNull());
    QVERIFY(imagesWithinChannelTolerance(before, after, 3));
}

void CoreTests::smartLayerTransformsRemainSourceBackedAndPersistSampling()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Source-Backed Smart Transform");
    settings.pixelSize = QSize(64, 48);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid rasterId = document.addRasterLayer();
    QVERIFY(!rasterId.isNull());
    QImage pixels(settings.pixelSize, QImage::Format_RGBA8888);
    pixels.fill(Qt::transparent);
    pixels.setColorSpace(document.sourceImage().colorSpace());
    for (int y = 0; y < pixels.height(); ++y) {
        uchar *row = pixels.scanLine(y);
        for (int x = 0; x < pixels.width(); ++x) {
            const int offset = x * 4;
            const bool checker = ((x / 2) + (y / 2)) & 1;
            row[offset] = static_cast<uchar>(checker ? 235 : 18);
            row[offset + 1] = static_cast<uchar>((x * 17 + y * 3) & 255);
            row[offset + 2] = static_cast<uchar>(checker ? 35 : 220);
            row[offset + 3] = static_cast<uchar>(80 + ((x * 5 + y * 7) % 176));
        }
    }
    // A transparent pixel with meaningful RGB exercises the straight-RGBA
    // Smart transform reference path rather than a premultiplied source bake.
    uchar *hidden = pixels.scanLine(6) + 7 * 4;
    hidden[0] = 203;
    hidden[1] = 61;
    hidden[2] = 149;
    hidden[3] = 0;
    QVERIFY(document.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.name = QStringLiteral("Authoritative Pattern");
        layer.rasterImage = pixels;
    }));

    const QUuid smartId = document.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    LayerNode smart = document.layerById(smartId);
    QCOMPARE(smart.type, LayerType::Smart);
    QCOMPARE(smart.smartTransform.interpolation, TransformInterpolation::Bilinear);
    const QUuid sourceId = smart.smartSource.sourceId;
    const SmartSourceDescriptor *source = document.smartSources().find(sourceId);
    QVERIFY(source != nullptr);
    const quint64 sourceRevisionBeforeTransform = source->revision;
    const QImage authoritativePresentationBefore = source->presentationImage.copy();
    const QJsonObject authoritativeDocumentBefore = source->embeddedDocument;

    // Add an instance mask so source and mask sampling take the same persisted
    // transform quality without changing the authoritative embedded source.
    QImage instanceMask(settings.pixelSize, QImage::Format_Grayscale8);
    for (int y = 0; y < instanceMask.height(); ++y) {
        uchar *row = instanceMask.scanLine(y);
        for (int x = 0; x < instanceMask.width(); ++x) {
            row[x] = static_cast<uchar>(std::clamp(x * 4 + (y & 7) * 3, 0, 255));
        }
    }
    QVERIFY(document.updateLayer(smartId, [&](LayerNode &layer) {
        layer.maskImage = instanceMask;
        layer.maskReferenceSize = settings.pixelSize;
        layer.maskReferenceOrigin = QPointF(0.0, 0.0);
        layer.maskEnabled = true;
        layer.maskInverted = false;
        layer.transform = QTransform::fromScale(0.20, 0.20);
        layer.smartTransform.interpolation = TransformInterpolation::Lanczos3;
    }));
    const QImage atTwentyPercent = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!atTwentyPercent.isNull());

    source = document.smartSources().find(sourceId);
    QVERIFY(source != nullptr);
    QCOMPARE(source->revision, sourceRevisionBeforeTransform);
    QVERIFY(exactImagesEqual(source->presentationImage, authoritativePresentationBefore));
    QCOMPARE(source->embeddedDocument, authoritativeDocumentBefore);

    // This models a second transform applied to the existing 20% instance.
    // The stored transform becomes 80%, but there is still no 20% raster bake.
    const QTransform accumulated = QTransform::fromScale(0.20, 0.20)
        * QTransform::fromScale(4.0, 4.0);
    QVERIFY(document.updateLayer(smartId, [&](LayerNode &layer) {
        layer.transform = accumulated;
        layer.smartTransform.interpolation = TransformInterpolation::Lanczos3;
    }));
    smart = document.layerById(smartId);
    QVERIFY(transformsClose(smart.transform, QTransform::fromScale(0.80, 0.80)));
    QCOMPARE(smart.smartTransform.interpolation, TransformInterpolation::Lanczos3);
    QVERIFY(smart.rasterImage.isNull());
    source = document.smartSources().find(sourceId);
    QVERIFY(source != nullptr);
    QCOMPARE(source->revision, sourceRevisionBeforeTransform);
    QVERIFY(exactImagesEqual(source->presentationImage, authoritativePresentationBefore));
    QCOMPARE(source->embeddedDocument, authoritativeDocumentBefore);

    const QImage atEightyPercent = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!atEightyPercent.isNull());
    QVERIFY(!exactImagesEqual(atTwentyPercent, atEightyPercent));

    // Editing the authoritative source after the transform must update the
    // transformed instance without changing its geometry or sampling method.
    PhotoDocument editor;
    QHash<QUuid, quint64> baseline;
    QVERIFY2(document.createEditableSmartSourceDocument(sourceId, &editor,
                                                         &baseline, &error),
             qPrintable(error));
    QImage editedPixels = pixels;
    editedPixels.detach();
    QPainter painter(&editedPixels);
    painter.fillRect(QRect(18, 12, 22, 17), QColor(20, 230, 95, 245));
    painter.end();
    QVERIFY(editor.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = editedPixels;
    }));
    QVERIFY2(document.commitEditableSmartSourceDocument(sourceId, editor, baseline,
                                                          nullptr, &error),
             qPrintable(error));
    const LayerNode afterSourceEdit = document.layerById(smartId);
    QVERIFY(transformsClose(afterSourceEdit.transform,
                            QTransform::fromScale(0.80, 0.80)));
    QCOMPARE(afterSourceEdit.smartTransform.interpolation,
             TransformInterpolation::Lanczos3);
    const SmartSourceDescriptor *editedSource = document.smartSources().find(sourceId);
    QVERIFY(editedSource != nullptr);
    QCOMPARE(editedSource->revision, sourceRevisionBeforeTransform + 1);
    QVERIFY(!exactImagesEqual(editedSource->presentationImage,
                              authoritativePresentationBefore));
    const QImage afterSourceEditRender = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!afterSourceEditRender.isNull());
    QVERIFY(!exactImagesEqual(atEightyPercent, afterSourceEditRender));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("smart-transform-v23.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument encoded = QJsonDocument::fromJson(file.readAll());
    QVERIFY(encoded.isObject());
    file.close();
    QCOMPARE(encoded.object().value(QStringLiteral("version")).toInt(), 27);
    QJsonArray encodedTree = encoded.object().value(QStringLiteral("layerTree")).toArray();
    QJsonObject encodedSmart;
    for (const QJsonValue &value : encodedTree) {
        const QJsonObject candidate = value.toObject();
        if (QUuid(candidate.value(QStringLiteral("id")).toString()) == smartId) {
            encodedSmart = candidate;
            break;
        }
    }
    QVERIFY(!encodedSmart.isEmpty());
    const QJsonObject encodedTransform = encodedSmart.value(
        QStringLiteral("smartTransform")).toObject();
    QCOMPARE(encodedTransform.value(QStringLiteral("schema")).toInt(), 1);
    QCOMPARE(encodedTransform.value(QStringLiteral("interpolation")).toInt(),
             static_cast<int>(TransformInterpolation::Lanczos3));

    PhotoDocument reopened;
    QVERIFY2(reopened.loadProject(path, &error), qPrintable(error));
    const LayerNode reopenedSmart = reopened.layerById(smartId);
    QVERIFY(transformsClose(reopenedSmart.transform,
                            QTransform::fromScale(0.80, 0.80)));
    QCOMPARE(reopenedSmart.smartTransform.interpolation,
             TransformInterpolation::Lanczos3);
    const QImage reopenedRender = ImageProcessor::renderPreservingHiddenRgb(
        reopened.sourceImage(), reopened.layers(), nullptr, reopened.sourceImage().size(),
        reopened.colourState().processingCompatibility);
    QVERIFY(imagesWithinChannelTolerance(afterSourceEditRender, reopenedRender, 1));

    // A project claiming to predate format 20 cannot smuggle in the new
    // persistent transform-quality state.
    QJsonObject forgedRoot = encoded.object();
    forgedRoot.insert(QStringLiteral("version"), 19);
    // Keep the embedded source envelope honest for v19 so this fixture tests
    // the root Smart transform anti-smuggling rule rather than the newer
    // schema-4 Live Filter envelope.
    QJsonArray forgedSources = forgedRoot.value(QStringLiteral("smartSources")).toArray();
    for (qsizetype i = 0; i < forgedSources.size(); ++i) {
        QJsonObject descriptor = forgedSources.at(i).toObject();
        QJsonObject embedded = descriptor.value(QStringLiteral("embeddedDocument")).toObject();
        if (!embedded.isEmpty()) {
            embedded.insert(QStringLiteral("schema"), 2);
            QJsonArray embeddedLayers = embedded.value(QStringLiteral("layers")).toArray();
            stripSmartTransformMetadata(&embeddedLayers);
            embedded.insert(QStringLiteral("layers"), embeddedLayers);
            descriptor.insert(QStringLiteral("embeddedDocument"), embedded);
            forgedSources.replace(i, descriptor);
        }
    }
    forgedRoot.insert(QStringLiteral("smartSources"), forgedSources);
    const QString forgedPath = directory.filePath(QStringLiteral("forged-v19-smart-transform.vfxphoto"));
    QFile forgedFile(forgedPath);
    QVERIFY(forgedFile.open(QIODevice::WriteOnly));
    QVERIFY(forgedFile.write(QJsonDocument(forgedRoot).toJson(QJsonDocument::Compact)) > 0);
    forgedFile.close();
    PhotoDocument forged;
    QString forgedError;
    QVERIFY(!forged.loadProject(forgedPath, &forgedError));
    QVERIFY(forgedError.contains(QStringLiteral("transform"), Qt::CaseInsensitive));

    // A genuine 0.14.0c-style project has no Smart transform metadata and
    // migrates to the historical Bilinear result.
    QJsonObject legacyRoot = encoded.object();
    legacyRoot.insert(QStringLiteral("version"), 19);
    QJsonArray legacyTree = legacyRoot.value(QStringLiteral("layerTree")).toArray();
    stripSmartTransformMetadata(&legacyTree);
    legacyRoot.insert(QStringLiteral("layerTree"), legacyTree);
    QJsonArray legacySources = legacyRoot.value(QStringLiteral("smartSources")).toArray();
    for (qsizetype i = 0; i < legacySources.size(); ++i) {
        QJsonObject descriptor = legacySources.at(i).toObject();
        QJsonObject embedded = descriptor.value(QStringLiteral("embeddedDocument")).toObject();
        if (!embedded.isEmpty()) {
            embedded.insert(QStringLiteral("schema"), 2);
            QJsonArray embeddedLayers = embedded.value(QStringLiteral("layers")).toArray();
            stripSmartTransformMetadata(&embeddedLayers);
            embedded.insert(QStringLiteral("layers"), embeddedLayers);
            descriptor.insert(QStringLiteral("embeddedDocument"), embedded);
            legacySources.replace(i, descriptor);
        }
    }
    legacyRoot.insert(QStringLiteral("smartSources"), legacySources);
    const QString legacyPath = directory.filePath(QStringLiteral("legacy-v19-smart-transform.vfxphoto"));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    QVERIFY(legacyFile.write(QJsonDocument(legacyRoot).toJson(QJsonDocument::Compact)) > 0);
    legacyFile.close();
    PhotoDocument migrated;
    QVERIFY2(migrated.loadProject(legacyPath, &error), qPrintable(error));
    QCOMPARE(migrated.layerById(smartId).smartTransform.interpolation,
             TransformInterpolation::Bilinear);
}


void CoreTests::smartLayerLiveFilterStackPersistsOrdersAndCachesCpuStages()
{
    SmartLayerTileCache &cache = SmartLayerTileCache::instance();
    cache.clear();

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Smart Live Filter Stack");
    settings.pixelSize = QSize(128, 96);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid backgroundId = document.addRasterLayer();
    QImage background(settings.pixelSize, QImage::Format_RGBA8888);
    background.fill(QColor(22, 28, 34, 255));
    background.setColorSpace(document.sourceImage().colorSpace());
    QVERIFY(document.updateLayer(backgroundId, [&](LayerNode &layer) {
        layer.rasterImage = background;
    }));

    const QUuid rasterId = document.addRasterLayer();
    QImage pixels(settings.pixelSize, QImage::Format_RGBA8888);
    pixels.fill(Qt::transparent);
    pixels.setColorSpace(document.sourceImage().colorSpace());
    QPainter painter(&pixels);
    painter.fillRect(QRect(18, 16, 66, 48), QColor(220, 70, 35, 220));
    painter.fillRect(QRect(48, 35, 58, 42), QColor(30, 190, 225, 170));
    painter.end();
    QVERIFY(document.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
    }));

    const QUuid smartId = document.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    const QImage unfiltered = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!unfiltered.isNull());

    const QUuid blurId = document.addLiveFilter(smartId, AdjustmentType::GaussianBlur, &error);
    QVERIFY2(!blurId.isNull(), qPrintable(error));
    AdjustmentData blur;
    blur.reset(AdjustmentType::GaussianBlur);
    auto blurParameters = std::get<GaussianBlurParameters>(blur.parameters);
    blurParameters.radius = 6.5;
    blurParameters.affectAlpha = true;
    blur.parameters = blurParameters;
    QVERIFY(document.updateLiveFilter(smartId, blurId, blur));

    const QUuid vibranceId = document.addLiveFilter(smartId, AdjustmentType::Vibrance, &error);
    QVERIFY2(!vibranceId.isNull(), qPrintable(error));
    AdjustmentData vibrance;
    vibrance.reset(AdjustmentType::Vibrance);
    auto vibranceParameters = std::get<VibranceParameters>(vibrance.parameters);
    vibranceParameters.vibrance = 55.0;
    vibranceParameters.saturation = 8.0;
    vibrance.parameters = vibranceParameters;
    QVERIFY(document.updateLiveFilter(smartId, vibranceId, vibrance));

    const LayerNode configured = document.layerById(smartId);
    QCOMPARE(configured.liveFilters.size(), 2);
    QCOMPARE(configured.liveFilters.at(0).id, blurId);
    QCOMPARE(configured.liveFilters.at(1).id, vibranceId);
    QVERIFY(configured.liveFilters.at(0).enabled);
    QVERIFY(configured.liveFilters.at(1).enabled);

    // Histogram/on-image analysis for a Live Filter must inspect the exact
    // input entering that filter: the transformed Smart source plus only the
    // preceding Live Filter prefix. It must not include the target/downstream
    // filters, the Smart instance mask, or unrelated parent layers.
    const QImage vibranceInput = ImageProcessor::renderLiveFilterInput(
        document.sourceImage(), document.layers(), smartId, vibranceId,
        document.sourceImage().size(), nullptr,
        document.colourState().processingCompatibility);
    QVERIFY(!vibranceInput.isNull());
    LayerNode blurPrefix = document.layerById(smartId);
    blurPrefix.liveFilters.resize(1);
    blurPrefix.opacity = 1.0;
    blurPrefix.blendMode = BlendMode::Copy;
    blurPrefix.maskImage = {};
    blurPrefix.maskReferenceSize = {};
    blurPrefix.maskReferenceOrigin = {};
    const QImage expectedVibranceInput = ImageProcessor::renderRegion(
        document.sourceImage(), {blurPrefix}, document.sourceImage().rect(),
        document.sourceImage().size(), nullptr,
        document.colourState().processingCompatibility)
        .convertToFormat(vibranceInput.format());
    QVERIFY(!expectedVibranceInput.isNull());
    QVERIFY(imagesWithinChannelTolerance(vibranceInput, expectedVibranceInput, 1));

    HistogramRequest liveFilterHistogram;
    liveFilterHistogram.documentSessionId = QUuid::createUuid();
    liveFilterHistogram.liveFilterOwnerId = smartId;
    liveFilterHistogram.liveFilterId = vibranceId;
    liveFilterHistogram.documentRevision = 1;
    liveFilterHistogram.colourStateRevision = document.colourStateRevision();
    liveFilterHistogram.processingCompatibility =
        document.colourState().processingCompatibility;
    liveFilterHistogram.source = document.sourceImage();
    liveFilterHistogram.layers = document.layers();
    liveFilterHistogram.documentSize = document.sourceImage().size();
    liveFilterHistogram.selection = document.selectionMask().snapshot();
    const HistogramData liveHistogram = HistogramService::calculate(liveFilterHistogram);
    QVERIFY(liveHistogram.isValid());
    QVERIFY(liveHistogram.adjustmentFound);
    QVERIFY(liveHistogram.includedPixels > 0);

    const QImage filtered = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!filtered.isNull());
    QVERIFY(!exactImagesEqual(unfiltered, filtered));
    const SmartLayerTileCache::Stats afterFiltered = cache.stats();
    QVERIFY(afterFiltered.transformedMisses >= 3); // source transform + two filter stages

    // A lower-layer edit changes the final composite but must reuse the Smart
    // transform and all unchanged Live Filter prefix stages.
    QImage changedBackground = background;
    changedBackground.detach();
    QPainter bgPainter(&changedBackground);
    bgPainter.fillRect(QRect(0, 0, 24, 24), QColor(95, 24, 120, 255));
    bgPainter.end();
    QVERIFY(document.updateLayer(backgroundId, [&](LayerNode &layer) {
        layer.rasterImage = changedBackground;
    }));
    const QImage afterLowerEdit = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!afterLowerEdit.isNull());
    const SmartLayerTileCache::Stats afterReuse = cache.stats();
    QVERIFY(afterReuse.transformedHits >= afterFiltered.transformedHits + 3);

    // Toggling is non-destructive and retains parameters/identity.
    QVERIFY(document.setLiveFilterEnabled(smartId, vibranceId, false));
    const QImage blurOnly = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!blurOnly.isNull());
    QVERIFY(!exactImagesEqual(afterLowerEdit, blurOnly));
    QVERIFY(document.setLiveFilterEnabled(smartId, vibranceId, true));
    const QImage restoredEnabled = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(imagesWithinChannelTolerance(afterLowerEdit, restoredEnabled, 1));

    // Per-filter masks are authored in the Smart instance reference space and
    // blend each filtered stage over its own input before the next filter.
    QVERIFY(document.addLiveFilterMask(smartId, vibranceId));
    QImage filterMask(settings.pixelSize, QImage::Format_Grayscale8);
    filterMask.fill(0);
    for (int y = 0; y < filterMask.height(); ++y) {
        uchar *row = filterMask.scanLine(y);
        std::fill(row, row + filterMask.width() / 2, static_cast<uchar>(255));
    }
    QVERIFY(document.updateLiveFilterMask(smartId, vibranceId, filterMask,
                                          settings.pixelSize, QPointF()));
    const QImage maskedVibrance = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!maskedVibrance.isNull());
    QVERIFY(!exactImagesEqual(restoredEnabled, maskedVibrance));

    QVERIFY(document.setLiveFilterMaskEnabled(smartId, vibranceId, false));
    const QImage disabledFilterMask = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(imagesWithinChannelTolerance(restoredEnabled, disabledFilterMask, 1));
    QVERIFY(document.setLiveFilterMaskEnabled(smartId, vibranceId, true));
    QVERIFY(document.setLiveFilterMaskInverted(smartId, vibranceId, true));
    const QImage invertedFilterMask = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!invertedFilterMask.isNull());
    QVERIFY(!exactImagesEqual(maskedVibrance, invertedFilterMask));
    QVERIFY(document.setLiveFilterMaskInverted(smartId, vibranceId, false));

    const LayerNode maskedStack = document.layerById(smartId);
    const auto maskedFilterIt = std::find_if(
        maskedStack.liveFilters.cbegin(), maskedStack.liveFilters.cend(),
        [vibranceId](const LiveFilter &filter) { return filter.id == vibranceId; });
    QVERIFY(maskedFilterIt != maskedStack.liveFilters.cend());
    QVERIFY(maskedFilterIt->hasMask());
    QCOMPARE(maskedFilterIt->maskReferenceSize, settings.pixelSize);
    QVERIFY(maskedFilterIt->maskEnabled);
    QVERIFY(!maskedFilterIt->maskInverted);

    // Reordering changes execution order but not filter identity or source.
    QVERIFY(document.moveLiveFilter(smartId, vibranceId, 0));
    LayerNode reordered = document.layerById(smartId);
    QCOMPARE(reordered.liveFilters.at(0).id, vibranceId);
    QCOMPARE(reordered.liveFilters.at(1).id, blurId);
    const QImage reorderedImage = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!reorderedImage.isNull());
    QVERIFY(!exactImagesEqual(restoredEnabled, reorderedImage));
    QVERIFY(document.moveLiveFilter(smartId, vibranceId, 1));

    // Add/remove is structural and does not disturb the surviving stack IDs.
    const QUuid temporaryId = document.addLiveFilter(smartId, AdjustmentType::HighPass, &error);
    QVERIFY2(!temporaryId.isNull(), qPrintable(error));
    QCOMPARE(document.layerById(smartId).liveFilters.size(), 3);
    QVERIFY(document.removeLiveFilter(smartId, temporaryId));
    const LayerNode afterRemove = document.layerById(smartId);
    QCOMPARE(afterRemove.liveFilters.size(), 2);
    QCOMPARE(afterRemove.liveFilters.at(0).id, blurId);
    QCOMPARE(afterRemove.liveFilters.at(1).id, vibranceId);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("live-filters-v23.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(json.isObject());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(), 27);
    const QJsonArray tree = json.object().value(QStringLiteral("layerTree")).toArray();
    QJsonObject encodedSmart;
    for (const QJsonValue &value : tree) {
        const QJsonObject candidate = value.toObject();
        if (QUuid(candidate.value(QStringLiteral("id")).toString()) == smartId) {
            encodedSmart = candidate;
            break;
        }
    }
    QVERIFY(encodedSmart.value(QStringLiteral("liveFilters")).isArray());
    const QJsonArray encodedFilters = encodedSmart.value(QStringLiteral("liveFilters")).toArray();
    QCOMPARE(encodedFilters.size(), 2);
    QJsonObject encodedMaskedFilter;
    for (const QJsonValue &value : encodedFilters) {
        const QJsonObject candidate = value.toObject();
        if (QUuid(candidate.value(QStringLiteral("id")).toString()) == vibranceId) {
            encodedMaskedFilter = candidate;
            break;
        }
    }
    QVERIFY(!encodedMaskedFilter.isEmpty());
    QCOMPARE(encodedMaskedFilter.value(QStringLiteral("schema")).toInt(), 2);
    QVERIFY(encodedMaskedFilter.value(QStringLiteral("maskImage")).isString());
    QVERIFY(!encodedMaskedFilter.value(QStringLiteral("maskImage")).toString().isEmpty());
    QCOMPARE(encodedMaskedFilter.value(QStringLiteral("maskEnabled")).toBool(), true);
    QCOMPARE(encodedMaskedFilter.value(QStringLiteral("maskInverted")).toBool(), false);

    PhotoDocument reopened;
    QVERIFY2(reopened.loadProject(path, &error), qPrintable(error));
    const LayerNode reopenedSmart = reopened.layerById(smartId);
    QVERIFY(reopenedSmart.liveFilters == document.layerById(smartId).liveFilters);
    const SmartSourceDescriptor *reopenedSource = reopened.smartSources().find(
        reopenedSmart.smartSource.sourceId);
    QVERIFY(reopenedSource != nullptr);
    QCOMPARE(reopenedSource->embeddedDocument.value(QStringLiteral("schema")).toInt(), 10);

    // 0.14.0f/project-v21 knew Live Filters but not per-filter masks. It must
    // reject mask metadata smuggled into that older envelope while accepting a
    // genuine schema-1 Live Filter stack with the mask fields removed.
    QJsonObject forgedMaskRoot = json.object();
    forgedMaskRoot.insert(QStringLiteral("version"), 21);
    QJsonArray forgedMaskSources = forgedMaskRoot.value(QStringLiteral("smartSources")).toArray();
    for (qsizetype index = 0; index < forgedMaskSources.size(); ++index) {
        QJsonObject descriptor = forgedMaskSources.at(index).toObject();
        QJsonObject embedded = descriptor.value(QStringLiteral("embeddedDocument")).toObject();
        if (!embedded.isEmpty()) {
            embedded.insert(QStringLiteral("schema"), 4);
            descriptor.insert(QStringLiteral("embeddedDocument"), embedded);
            forgedMaskSources.replace(index, descriptor);
        }
    }
    forgedMaskRoot.insert(QStringLiteral("smartSources"), forgedMaskSources);
    const QString forgedMaskPath = directory.filePath(
        QStringLiteral("forged-v21-live-filter-mask.vfxphoto"));
    QFile forgedMaskFile(forgedMaskPath);
    QVERIFY(forgedMaskFile.open(QIODevice::WriteOnly));
    QVERIFY(forgedMaskFile.write(QJsonDocument(forgedMaskRoot).toJson(QJsonDocument::Compact)) > 0);
    forgedMaskFile.close();
    PhotoDocument forgedMask;
    QString forgedMaskError;
    QVERIFY(!forgedMask.loadProject(forgedMaskPath, &forgedMaskError));
    QVERIFY(forgedMaskError.contains(QStringLiteral("mask"), Qt::CaseInsensitive)
            || forgedMaskError.contains(QStringLiteral("Live Filter"), Qt::CaseInsensitive));

    QJsonObject legacyV21Root = forgedMaskRoot;
    QJsonArray legacyV21Tree = legacyV21Root.value(QStringLiteral("layerTree")).toArray();
    QVERIFY(mutateLayerObject(&legacyV21Tree, smartId, [](QJsonObject &object) {
        QJsonArray filters = object.value(QStringLiteral("liveFilters")).toArray();
        for (qsizetype index = 0; index < filters.size(); ++index) {
            QJsonObject filter = filters.at(index).toObject();
            filter.insert(QStringLiteral("schema"), 1);
            filter.remove(QStringLiteral("maskImage"));
            filter.remove(QStringLiteral("maskReferenceWidth"));
            filter.remove(QStringLiteral("maskReferenceHeight"));
            filter.remove(QStringLiteral("maskReferenceOriginX"));
            filter.remove(QStringLiteral("maskReferenceOriginY"));
            filter.remove(QStringLiteral("maskEnabled"));
            filter.remove(QStringLiteral("maskInverted"));
            filters.replace(index, filter);
        }
        object.insert(QStringLiteral("liveFilters"), filters);
    }));
    legacyV21Root.insert(QStringLiteral("layerTree"), legacyV21Tree);
    const QString legacyV21Path = directory.filePath(
        QStringLiteral("legacy-v21-live-filter-no-mask.vfxphoto"));
    QFile legacyV21File(legacyV21Path);
    QVERIFY(legacyV21File.open(QIODevice::WriteOnly));
    QVERIFY(legacyV21File.write(QJsonDocument(legacyV21Root).toJson(QJsonDocument::Compact)) > 0);
    legacyV21File.close();
    PhotoDocument migratedV21;
    QVERIFY2(migratedV21.loadProject(legacyV21Path, &error), qPrintable(error));
    QCOMPARE(migratedV21.layerById(smartId).liveFilters.size(), 2);
    for (const LiveFilter &filter : migratedV21.layerById(smartId).liveFilters) {
        QVERIFY(!filter.hasMask());
    }

    // An older project cannot claim Live Filter metadata that did not exist in
    // its schema. A genuine v20 payload without Live Filters migrates empty.
    QJsonObject forgedRoot = json.object();
    forgedRoot.insert(QStringLiteral("version"), 20);
    const QString forgedPath = directory.filePath(QStringLiteral("forged-v20-live-filter.vfxphoto"));
    QFile forgedFile(forgedPath);
    QVERIFY(forgedFile.open(QIODevice::WriteOnly));
    QVERIFY(forgedFile.write(QJsonDocument(forgedRoot).toJson(QJsonDocument::Compact)) > 0);
    forgedFile.close();
    PhotoDocument forged;
    QString forgedError;
    QVERIFY(!forged.loadProject(forgedPath, &forgedError));
    QVERIFY(forgedError.contains(QStringLiteral("Live Filter"), Qt::CaseInsensitive));

    QJsonObject legacyRoot = json.object();
    legacyRoot.insert(QStringLiteral("version"), 20);
    QJsonArray legacyTree = legacyRoot.value(QStringLiteral("layerTree")).toArray();
    QVERIFY(mutateLayerObject(&legacyTree, smartId, [](QJsonObject &object) {
        object.remove(QStringLiteral("liveFilters"));
    }));
    legacyRoot.insert(QStringLiteral("layerTree"), legacyTree);
    QJsonArray sources = legacyRoot.value(QStringLiteral("smartSources")).toArray();
    for (qsizetype index = 0; index < sources.size(); ++index) {
        QJsonObject descriptor = sources.at(index).toObject();
        QJsonObject embedded = descriptor.value(QStringLiteral("embeddedDocument")).toObject();
        if (!embedded.isEmpty()) {
            embedded.insert(QStringLiteral("schema"), 3);
            descriptor.insert(QStringLiteral("embeddedDocument"), embedded);
            sources.replace(index, descriptor);
        }
    }
    legacyRoot.insert(QStringLiteral("smartSources"), sources);
    const QString legacyPath = directory.filePath(QStringLiteral("legacy-v20-no-live-filter.vfxphoto"));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    QVERIFY(legacyFile.write(QJsonDocument(legacyRoot).toJson(QJsonDocument::Compact)) > 0);
    legacyFile.close();
    PhotoDocument migrated;
    QVERIFY2(migrated.loadProject(legacyPath, &error), qPrintable(error));
    QVERIFY(migrated.layerById(smartId).liveFilters.isEmpty());

    cache.clear();
}


void CoreTests::layerEffectFoundationPersistsFxStackAndCoverageContract()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Layer Effect Foundation");
    settings.pixelSize = QSize(64, 48);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid rasterId = document.addRasterLayer();
    QImage pixels(settings.pixelSize, QImage::Format_RGBA64);
    pixels.fill(Qt::transparent);
    pixels.setColorSpace(document.sourceImage().colorSpace());
    QPainter painter(&pixels);
    painter.fillRect(QRect(8, 7, 42, 30), QColor(210, 65, 35, 230));
    painter.end();
    // Meaningful RGB beneath zero alpha must remain available to future fx
    // renderers even though it contributes no coverage.
    pixels.setPixelColor(54, 12, QColor(190, 60, 220, 0));
    QImage mask(settings.pixelSize, QImage::Format_Grayscale8);
    mask.fill(255);
    QPainter maskPainter(&mask);
    maskPainter.fillRect(QRect(8, 7, 18, 30), Qt::black);
    maskPainter.end();
    QVERIFY(document.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.name = QStringLiteral("Effect Source");
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = settings.pixelSize;
        layer.rasterReferenceOrigin = {};
        layer.maskImage = mask;
        layer.maskReferenceSize = settings.pixelSize;
        layer.maskReferenceOrigin = {};
        layer.maskEnabled = true;
        layer.maskInverted = false;
    }));

    const LayerEffectInputRegion inputs = ImageProcessor::renderLayerEffectInputRegion(
        document.sourceImage(), document.layers(), rasterId,
        document.sourceImage().rect(), document.sourceImage().size(), nullptr,
        document.colourState().processingCompatibility);
    QVERIFY(inputs.isValid());
    QCOMPARE(inputs.coverage.format(), QImage::Format_Grayscale16);
    const QImage content64 = inputs.content.convertToFormat(QImage::Format_RGBA64);
    const QRgba64 hidden = reinterpret_cast<const QRgba64 *>(
        content64.constScanLine(12))[54];
    QCOMPARE(hidden.alpha(), static_cast<quint16>(0));
    QVERIFY(hidden.red() > 0 && hidden.blue() > 0);
    const QImage content = inputs.content.convertToFormat(QImage::Format_RGBA8888);
    const QImage coverage = inputs.coverage.convertToFormat(QImage::Format_Grayscale8);
    QVERIFY(content.pixelColor(12, 12).alpha() > 0);
    QCOMPARE(coverage.constScanLine(12)[12], static_cast<uchar>(0));
    QVERIFY(content.pixelColor(36, 12).alpha() > 0);
    QVERIFY(coverage.constScanLine(12)[36] > 0);

    const QUuid shadowId = document.addLayerEffect(
        rasterId, LayerEffectType::DropShadow, &error);
    QVERIFY2(!shadowId.isNull(), qPrintable(error));
    LayerNode configured = document.layerById(rasterId);
    QCOMPARE(configured.layerEffects.size(), 1);
    QVERIFY(configured.layerEffects.at(0).enabled);
    QVERIFY(layerEffectStackRenderIdentity(configured.layerEffects)
            != layerEffectStackRenderIdentity(QVector<LayerEffect> {}));
    QVERIFY(layerEffectStackSpatialRadius2D(configured.layerEffects).width() > 0);

    LayerEffect configuredShadow = configured.layerEffects.at(0);
    configuredShadow.effectOpacity = 1.0;
    configuredShadow.effectBlendMode = BlendMode::Multiply;
    configuredShadow.angleDegrees = 180.0;
    configuredShadow.distance = 8.0;
    configuredShadow.spread = 0.0;
    configuredShadow.size = 0.0;
    QVERIFY(document.updateLayerEffect(rasterId, shadowId, configuredShadow, &error));

    const QVector<LayerEffectRenderPass> shadowPasses = ImageProcessor::renderLayerEffectPasses(
        document.sourceImage(), document.layerById(rasterId), document.sourceImage().rect(),
        document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
        document.colourState().processingCompatibility);
    QCOMPARE(shadowPasses.size(), 1);
    QVERIFY(shadowPasses.constFirst().behindSource);
    QVERIFY(shadowPasses.constFirst().image.depth() > 32);
    const QImage shadowImage = shadowPasses.constFirst().image.convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(shadowImage.pixelColor(55, 20).alpha() > 0);
    QCOMPARE(shadowImage.pixelColor(24, 20).alpha(), 0);

    // The expensive generated fx pass belongs to the owner layer, not the
    // pixels beneath it. A lower-layer metadata edit must therefore reuse the
    // exact same cached pass.
    const quint64 shadowPassCacheKey = shadowPasses.constFirst().image.cacheKey();
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.name = QStringLiteral("Lower Layer Metadata Edit");
    }));
    const QVector<LayerEffectRenderPass> reusedShadowPasses =
        ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), document.layerById(rasterId),
            document.sourceImage().rect(), document.sourceImage().size(),
            document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    QCOMPARE(reusedShadowPasses.size(), 1);
    QCOMPARE(reusedShadowPasses.constFirst().image.cacheKey(), shadowPassCacheKey);

    const QImage composedShadow = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!composedShadow.isNull());
    QVERIFY(composedShadow.pixelColor(55, 20).alpha() > 0);

    // The same generated-pass contract must remain depth-correct in 8-bit documents.
    QImage source8(QSize(40, 32), QImage::Format_ARGB32_Premultiplied);
    source8.fill(Qt::transparent);
    LayerNode raster8;
    raster8.type = LayerType::Raster;
    raster8.rasterImage = QImage(source8.size(), QImage::Format_RGBA8888);
    raster8.rasterImage.fill(Qt::transparent);
    QPainter painter8(&raster8.rasterImage);
    painter8.fillRect(QRect(10, 8, 12, 12), QColor(220, 120, 40, 255));
    painter8.end();
    raster8.rasterReferenceSize = source8.size();
    LayerEffect glow8;
    glow8.type = LayerEffectType::OuterGlow;
    glow8.enabled = true;
    glow8.effectBlendMode = BlendMode::Screen;
    glow8.effectOpacity = 1.0;
    glow8.size = 4.0;
    glow8.spread = 20.0;
    glow8.normalise();
    raster8.layerEffects = {glow8};
    const QVector<LayerEffectRenderPass> passes8 = ImageProcessor::renderLayerEffectPasses(
        source8, raster8, source8.rect(), source8.size(), QTransform(), nullptr,
        ColourProcessingCompatibility::LegacyV1);
    QCOMPARE(passes8.size(), 1);
    QVERIFY(passes8.constFirst().image.depth() <= 32);

    LayerEffect stroke8;
    stroke8.type = LayerEffectType::Stroke;
    stroke8.enabled = true;
    stroke8.size = 2.0;
    stroke8.strokePosition = LayerEffectStrokePosition::Outside;
    stroke8.normalise();
    LayerEffect colourOverlay8;
    colourOverlay8.type = LayerEffectType::ColourOverlay;
    colourOverlay8.enabled = true;
    colourOverlay8.colour = QColor(40, 210, 100);
    colourOverlay8.effectOpacity = 0.8;
    colourOverlay8.normalise();
    LayerEffect gradientOverlay8;
    gradientOverlay8.type = LayerEffectType::GradientOverlay;
    gradientOverlay8.enabled = true;
    gradientOverlay8.gradientStops = {{0.0, QColor(255, 0, 0)},
                                      {1.0, QColor(0, 0, 255)}};
    gradientOverlay8.gradientAngleDegrees = 0.0;
    gradientOverlay8.normalise();
    raster8.layerEffects = {stroke8, colourOverlay8, gradientOverlay8};
    const QVector<LayerEffectRenderPass> overlayPasses8 =
        ImageProcessor::renderLayerEffectPasses(
            source8, raster8, source8.rect(), source8.size(), QTransform(), nullptr,
            ColourProcessingCompatibility::LegacyV1);
    QCOMPARE(overlayPasses8.size(), 3);
    for (const LayerEffectRenderPass &pass : overlayPasses8) {
        QVERIFY(pass.image.depth() <= 32);
    }
    LayerEffect bevel8;
    bevel8.type = LayerEffectType::BevelEmboss;
    bevel8.enabled = true;
    bevel8.size = 4.0;
    bevel8.bevelDepth = 150.0;
    bevel8.bevelSoften = 1.0;
    bevel8.normalise();
    raster8.layerEffects = {bevel8};
    const QVector<LayerEffectRenderPass> bevelPasses8 =
        ImageProcessor::renderLayerEffectPasses(
            source8, raster8, source8.rect(), source8.size(), QTransform(), nullptr,
            ColourProcessingCompatibility::LegacyV1);
    QCOMPARE(bevelPasses8.size(), 2);
    for (const LayerEffectRenderPass &pass : bevelPasses8) {
        QVERIFY(pass.image.depth() <= 32);
    }

    const QUuid strokeId = document.addLayerEffect(
        rasterId, LayerEffectType::Stroke, &error);
    QVERIFY2(!strokeId.isNull(), qPrintable(error));
    configured = document.layerById(rasterId);
    QCOMPARE(configured.layerEffects.size(), 2);
    QVERIFY(configured.layerEffects.at(1).enabled);
    QCOMPARE(configured.layerEffects.at(1).size, 3.0);
    QCOMPARE(configured.layerEffects.at(1).strokePosition,
             LayerEffectStrokePosition::Outside);

    const auto renderStrokePass = [&](const LayerEffectStrokePosition position) {
        LayerNode strokeLayer = document.layerById(rasterId);
        LayerEffect stroke = strokeLayer.layerEffects.at(1);
        stroke.strokePosition = position;
        stroke.size = 4.0;
        stroke.colour = QColor(20, 80, 240);
        stroke.effectOpacity = 1.0;
        stroke.normalise();
        strokeLayer.layerEffects = {stroke};
        return ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), strokeLayer, document.sourceImage().rect(),
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    };
    const QVector<LayerEffectRenderPass> outsideStroke = renderStrokePass(
        LayerEffectStrokePosition::Outside);
    QCOMPARE(outsideStroke.size(), 1);
    QVERIFY(outsideStroke.constFirst().behindSource);
    QVERIFY(outsideStroke.constFirst().image.pixelColor(53, 20).alpha() > 0);
    const QVector<LayerEffectRenderPass> insideStroke = renderStrokePass(
        LayerEffectStrokePosition::Inside);
    QCOMPARE(insideStroke.size(), 1);
    QVERIFY(!insideStroke.constFirst().behindSource);
    QCOMPARE(insideStroke.constFirst().image.pixelColor(53, 20).alpha(), 0);
    const QVector<LayerEffectRenderPass> centreStroke = renderStrokePass(
        LayerEffectStrokePosition::Centre);
    QCOMPARE(centreStroke.size(), 1);
    QVERIFY(!centreStroke.constFirst().behindSource);

    const QUuid colourOverlayId = document.addLayerEffect(
        rasterId, LayerEffectType::ColourOverlay, &error);
    const QUuid gradientOverlayId = document.addLayerEffect(
        rasterId, LayerEffectType::GradientOverlay, &error);
    QVERIFY(!colourOverlayId.isNull() && !gradientOverlayId.isNull());
    LayerNode overlayLayer = document.layerById(rasterId);
    const auto colourOverlayIt = std::find_if(
        overlayLayer.layerEffects.begin(), overlayLayer.layerEffects.end(),
        [colourOverlayId](const LayerEffect &effect) { return effect.id == colourOverlayId; });
    QVERIFY(colourOverlayIt != overlayLayer.layerEffects.end());
    LayerEffect colourOverlay = *colourOverlayIt;
    colourOverlay.colour = QColor(15, 210, 120);
    colourOverlay.effectOpacity = 1.0;
    colourOverlay.effectBlendMode = BlendMode::Copy;
    colourOverlay.normalise();
    QVERIFY(document.updateLayerEffect(rasterId, colourOverlayId, colourOverlay, &error));
    LayerNode onlyColourOverlayLayer = document.layerById(rasterId);
    onlyColourOverlayLayer.layerEffects = {colourOverlay};
    const QVector<LayerEffectRenderPass> colourOverlayPasses =
        ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), onlyColourOverlayLayer, document.sourceImage().rect(),
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    QCOMPARE(colourOverlayPasses.size(), 1);
    QVERIFY(!colourOverlayPasses.constFirst().behindSource);
    QCOMPARE(colourOverlayPasses.constFirst().image.pixelColor(12, 20).alpha(), 0);
    const QColor overlayCovered = colourOverlayPasses.constFirst().image.pixelColor(36, 20);
    QVERIFY(overlayCovered.alpha() > 0);
    QVERIFY(overlayCovered.green() > overlayCovered.red());

    overlayLayer = document.layerById(rasterId);
    const auto gradientIt = std::find_if(
        overlayLayer.layerEffects.begin(), overlayLayer.layerEffects.end(),
        [gradientOverlayId](const LayerEffect &effect) { return effect.id == gradientOverlayId; });
    QVERIFY(gradientIt != overlayLayer.layerEffects.end());
    LayerEffect gradient = *gradientIt;
    gradient.effectOpacity = 1.0;
    gradient.gradientStops = {{0.0, QColor(255, 0, 0)},
                              {0.5, QColor(0, 255, 0)},
                              {1.0, QColor(0, 0, 255)}};
    gradient.gradientStyle = LayerEffectGradientStyle::Linear;
    gradient.gradientAngleDegrees = 0.0;
    gradient.gradientScale = 100.0;
    gradient.gradientReverse = false;
    gradient.normalise();
    QVERIFY(document.updateLayerEffect(rasterId, gradientOverlayId, gradient, &error));

    LayerNode onlyGradientLayer = document.layerById(rasterId);
    onlyGradientLayer.layerEffects = {gradient};
    const QVector<LayerEffectRenderPass> gradientPasses = ImageProcessor::renderLayerEffectPasses(
        document.sourceImage(), onlyGradientLayer, document.sourceImage().rect(),
        document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
        document.colourState().processingCompatibility);
    QCOMPARE(gradientPasses.size(), 1);
    QVERIFY(!gradientPasses.constFirst().behindSource);
    const QColor gradientLeft = gradientPasses.constFirst().image.pixelColor(28, 20);
    const QColor gradientRight = gradientPasses.constFirst().image.pixelColor(46, 20);
    QVERIFY(gradientLeft.alpha() > 0 && gradientRight.alpha() > 0);
    QVERIFY(gradientLeft != gradientRight);

    // Gradient coordinates are anchored to stable owner bounds rather than to
    // the requested render tile. Two independently rendered horizontal tiles
    // must stitch to the exact same pixels as the full-region pass.
    const QRect leftHalf(0, 0, settings.pixelSize.width() / 2, settings.pixelSize.height());
    const QRect rightHalf(leftHalf.width(), 0,
                          settings.pixelSize.width() - leftHalf.width(),
                          settings.pixelSize.height());
    const QVector<LayerEffectRenderPass> gradientLeftPass =
        ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), onlyGradientLayer, leftHalf,
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    const QVector<LayerEffectRenderPass> gradientRightPass =
        ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), onlyGradientLayer, rightHalf,
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    QCOMPARE(gradientLeftPass.size(), 1);
    QCOMPARE(gradientRightPass.size(), 1);
    QImage stitchedGradient(settings.pixelSize, gradientPasses.constFirst().image.format());
    stitchedGradient.fill(Qt::transparent);
    stitchedGradient.setColorSpace(gradientPasses.constFirst().image.colorSpace());
    stitchedGradient.setDevicePixelRatio(gradientPasses.constFirst().image.devicePixelRatio());
    stitchedGradient.setDotsPerMeterX(gradientPasses.constFirst().image.dotsPerMeterX());
    stitchedGradient.setDotsPerMeterY(gradientPasses.constFirst().image.dotsPerMeterY());
    const qsizetype gradientBytesPerPixel = stitchedGradient.depth() / 8;
    for (int y = 0; y < stitchedGradient.height(); ++y) {
        std::memcpy(stitchedGradient.scanLine(y),
                    gradientLeftPass.constFirst().image.constScanLine(y),
                    static_cast<std::size_t>(leftHalf.width() * gradientBytesPerPixel));
        std::memcpy(stitchedGradient.scanLine(y)
                        + leftHalf.width() * gradientBytesPerPixel,
                    gradientRightPass.constFirst().image.constScanLine(y),
                    static_cast<std::size_t>(rightHalf.width() * gradientBytesPerPixel));
    }
    QVERIFY(exactImagesEqual(stitchedGradient, gradientPasses.constFirst().image));

    LayerEffect radialGradient = gradient;
    radialGradient.gradientStyle = LayerEffectGradientStyle::Radial;
    radialGradient.gradientReverse = false;
    radialGradient.normalise();
    LayerNode radialGradientLayer = onlyGradientLayer;
    radialGradientLayer.layerEffects = {radialGradient};
    const QVector<LayerEffectRenderPass> radialPasses = ImageProcessor::renderLayerEffectPasses(
        document.sourceImage(), radialGradientLayer, document.sourceImage().rect(),
        document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
        document.colourState().processingCompatibility);
    QCOMPARE(radialPasses.size(), 1);
    const QColor radialCentre = radialPasses.constFirst().image.pixelColor(38, 22);
    const QColor radialEdge = radialPasses.constFirst().image.pixelColor(48, 22);
    QVERIFY(radialCentre.alpha() > 0 && radialEdge.alpha() > 0);
    QVERIFY(radialCentre != radialEdge);
    radialGradient.gradientReverse = true;
    radialGradient.normalise();
    radialGradientLayer.layerEffects = {radialGradient};
    const QVector<LayerEffectRenderPass> reversedRadialPasses =
        ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), radialGradientLayer, document.sourceImage().rect(),
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    QCOMPARE(reversedRadialPasses.size(), 1);
    QVERIFY(reversedRadialPasses.constFirst().image.pixelColor(38, 22) != radialCentre);

    const QUuid bevelId = document.addLayerEffect(
        rasterId, LayerEffectType::BevelEmboss, &error);
    QVERIFY2(!bevelId.isNull(), qPrintable(error));
    LayerNode bevelOwner = document.layerById(rasterId);
    auto bevelIt = std::find_if(
        bevelOwner.layerEffects.begin(), bevelOwner.layerEffects.end(),
        [bevelId](const LayerEffect &effect) { return effect.id == bevelId; });
    QVERIFY(bevelIt != bevelOwner.layerEffects.end());
    LayerEffect bevel = *bevelIt;
    bevel.bevelStyle = LayerEffectBevelStyle::InnerBevel;
    bevel.bevelDirection = LayerEffectBevelDirection::Up;
    bevel.bevelDepth = 180.0;
    bevel.size = 7.0;
    bevel.bevelSoften = 1.5;
    bevel.angleDegrees = 135.0;
    bevel.bevelAltitudeDegrees = 30.0;
    bevel.bevelHighlightColour = QColor(255, 245, 220);
    bevel.bevelHighlightBlendMode = BlendMode::Screen;
    bevel.bevelHighlightOpacity = 0.8;
    bevel.bevelShadowColour = QColor(35, 25, 20);
    bevel.bevelShadowBlendMode = BlendMode::Multiply;
    bevel.bevelShadowOpacity = 0.7;
    bevel.normalise();
    QVERIFY(document.updateLayerEffect(rasterId, bevelId, bevel, &error));
    const auto renderBevel = [&](const LayerEffectBevelStyle style,
                                 const LayerEffectBevelDirection direction) {
        LayerNode owner = document.layerById(rasterId);
        auto found = std::find_if(owner.layerEffects.begin(), owner.layerEffects.end(),
                                  [bevelId](const LayerEffect &effect) { return effect.id == bevelId; });
        if (found == owner.layerEffects.end()) {
            return QVector<LayerEffectRenderPass>();
        }
        LayerEffect configuredBevel = *found;
        configuredBevel.bevelStyle = style;
        configuredBevel.bevelDirection = direction;
        configuredBevel.normalise();
        owner.layerEffects = {configuredBevel};
        return ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), owner, document.sourceImage().rect(),
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    };
    const auto alphaTotal = [](const QImage &image) {
        const QImage rgba = image.convertToFormat(QImage::Format_RGBA64);
        quint64 total = 0;
        for (int y = 0; y < rgba.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(y));
            for (int x = 0; x < rgba.width(); ++x) total += row[x].alpha();
        }
        return total;
    };
    const QVector<LayerEffectRenderPass> innerBevelPasses = renderBevel(
        LayerEffectBevelStyle::InnerBevel, LayerEffectBevelDirection::Up);
    QCOMPARE(innerBevelPasses.size(), 2);
    QVERIFY(!innerBevelPasses.at(0).behindSource && !innerBevelPasses.at(1).behindSource);
    QVERIFY(innerBevelPasses.at(0).image.depth() > 32);
    QVERIFY(alphaTotal(innerBevelPasses.at(0).image) > 0);
    QVERIFY(alphaTotal(innerBevelPasses.at(1).image) > 0);
    const quint64 bevelHighlightCacheKey = innerBevelPasses.at(0).image.cacheKey();
    const quint64 bevelShadowCacheKey = innerBevelPasses.at(1).image.cacheKey();
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.name = QStringLiteral("Another Lower-Layer Metadata Edit");
    }));
    const QVector<LayerEffectRenderPass> reusedBevelPasses = renderBevel(
        LayerEffectBevelStyle::InnerBevel, LayerEffectBevelDirection::Up);
    QCOMPARE(reusedBevelPasses.size(), 2);
    QCOMPARE(reusedBevelPasses.at(0).image.cacheKey(), bevelHighlightCacheKey);
    QCOMPARE(reusedBevelPasses.at(1).image.cacheKey(), bevelShadowCacheKey);
    const QVector<LayerEffectRenderPass> downBevelPasses = renderBevel(
        LayerEffectBevelStyle::InnerBevel, LayerEffectBevelDirection::Down);
    QCOMPARE(downBevelPasses.size(), 2);
    QVERIFY(!exactImagesEqual(innerBevelPasses.at(0).image, downBevelPasses.at(0).image));

    // Bevel evaluates from an effect-expanded coverage footprint, so independent
    // render tiles must stitch to the exact same highlight/shadow pixels as a
    // single full-region request. This guards signed-distance and soften halos.
    LayerNode tiledBevelLayer = document.layerById(rasterId);
    auto tiledBevelIt = std::find_if(
        tiledBevelLayer.layerEffects.begin(), tiledBevelLayer.layerEffects.end(),
        [bevelId](const LayerEffect &effect) { return effect.id == bevelId; });
    QVERIFY(tiledBevelIt != tiledBevelLayer.layerEffects.end());
    LayerEffect tiledBevel = *tiledBevelIt;
    tiledBevel.bevelStyle = LayerEffectBevelStyle::InnerBevel;
    tiledBevel.bevelDirection = LayerEffectBevelDirection::Up;
    tiledBevel.normalise();
    tiledBevelLayer.layerEffects = {tiledBevel};
    const QRect bevelLeftHalf(0, 0, settings.pixelSize.width() / 2,
                              settings.pixelSize.height());
    const QRect bevelRightHalf(bevelLeftHalf.width(), 0,
                               settings.pixelSize.width() - bevelLeftHalf.width(),
                               settings.pixelSize.height());
    const QVector<LayerEffectRenderPass> bevelLeftPasses =
        ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), tiledBevelLayer, bevelLeftHalf,
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    const QVector<LayerEffectRenderPass> bevelRightPasses =
        ImageProcessor::renderLayerEffectPasses(
            document.sourceImage(), tiledBevelLayer, bevelRightHalf,
            document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
            document.colourState().processingCompatibility);
    QCOMPARE(bevelLeftPasses.size(), 2);
    QCOMPARE(bevelRightPasses.size(), 2);
    for (int passIndex = 0; passIndex < 2; ++passIndex) {
        QImage stitched(settings.pixelSize, innerBevelPasses.at(passIndex).image.format());
        stitched.fill(Qt::transparent);
        stitched.setColorSpace(innerBevelPasses.at(passIndex).image.colorSpace());
        const qsizetype bytesPerPixel = stitched.depth() / 8;
        for (int y = 0; y < stitched.height(); ++y) {
            std::memcpy(stitched.scanLine(y),
                        bevelLeftPasses.at(passIndex).image.constScanLine(y),
                        static_cast<std::size_t>(bevelLeftHalf.width() * bytesPerPixel));
            std::memcpy(stitched.scanLine(y)
                            + bevelLeftHalf.width() * bytesPerPixel,
                        bevelRightPasses.at(passIndex).image.constScanLine(y),
                        static_cast<std::size_t>(bevelRightHalf.width() * bytesPerPixel));
        }
        QVERIFY(exactImagesEqual(stitched, innerBevelPasses.at(passIndex).image));
    }
    for (const LayerEffectBevelStyle style : {LayerEffectBevelStyle::OuterBevel,
                                              LayerEffectBevelStyle::Emboss,
                                              LayerEffectBevelStyle::PillowEmboss}) {
        const QVector<LayerEffectRenderPass> stylePasses = renderBevel(
            style, LayerEffectBevelDirection::Up);
        QCOMPARE(stylePasses.size(), 2);
        QVERIFY(alphaTotal(stylePasses.at(0).image) > 0);
        QVERIFY(alphaTotal(stylePasses.at(1).image) > 0);
    }
    QVERIFY(layerEffectStackSpatialRadius2D({bevel}).width() >= qCeil(bevel.size));

    const QUuid innerShadowId = document.addLayerEffect(
        rasterId, LayerEffectType::InnerShadow, &error);
    const QUuid outerGlowId = document.addLayerEffect(
        rasterId, LayerEffectType::OuterGlow, &error);
    const QUuid innerGlowId = document.addLayerEffect(
        rasterId, LayerEffectType::InnerGlow, &error);
    QVERIFY(!innerShadowId.isNull() && !outerGlowId.isNull() && !innerGlowId.isNull());
    const QVector<LayerEffectRenderPass> allPasses = ImageProcessor::renderLayerEffectPasses(
        document.sourceImage(), document.layerById(rasterId), document.sourceImage().rect(),
        document.sourceImage().size(), document.layerWorldTransform(rasterId), nullptr,
        document.colourState().processingCompatibility);
    QCOMPARE(allPasses.size(), 9);
    QCOMPARE(std::count_if(allPasses.cbegin(), allPasses.cend(),
                           [](const LayerEffectRenderPass &pass) { return pass.behindSource; }), 3);

    QString enableError;
    QVERIFY(document.setLayerEffectEnabled(rasterId, strokeId, false, &enableError));
    QVERIFY(document.setLayerEffectEnabled(rasterId, strokeId, true, &enableError));

    // 0.14.0h schema-1 entries migrate deterministically to the current schema but stay
    // disabled so merely opening an older project never changes appearance.
    LayerEffect legacyDefinition;
    legacyDefinition.type = LayerEffectType::DropShadow;
    legacyDefinition.enabled = false;
    QJsonObject legacyEffectObject = legacyDefinition.toJson();
    legacyEffectObject.insert(QStringLiteral("schema"), 1);
    legacyEffectObject.remove(QStringLiteral("colour"));
    legacyEffectObject.remove(QStringLiteral("opacity"));
    legacyEffectObject.remove(QStringLiteral("blendMode"));
    legacyEffectObject.remove(QStringLiteral("angle"));
    legacyEffectObject.remove(QStringLiteral("distance"));
    legacyEffectObject.remove(QStringLiteral("spread"));
    legacyEffectObject.remove(QStringLiteral("size"));
    legacyEffectObject.remove(QStringLiteral("strokePosition"));
    legacyEffectObject.remove(QStringLiteral("gradientStops"));
    legacyEffectObject.remove(QStringLiteral("gradientInterpolation"));
    legacyEffectObject.remove(QStringLiteral("gradientStyle"));
    legacyEffectObject.remove(QStringLiteral("gradientAngle"));
    legacyEffectObject.remove(QStringLiteral("gradientScale"));
    legacyEffectObject.remove(QStringLiteral("gradientReverse"));
    legacyEffectObject.remove(QStringLiteral("bevelStyle"));
    legacyEffectObject.remove(QStringLiteral("bevelDirection"));
    legacyEffectObject.remove(QStringLiteral("bevelDepth"));
    legacyEffectObject.remove(QStringLiteral("bevelSoften"));
    legacyEffectObject.remove(QStringLiteral("bevelAltitude"));
    legacyEffectObject.remove(QStringLiteral("bevelHighlightColour"));
    legacyEffectObject.remove(QStringLiteral("bevelHighlightBlendMode"));
    legacyEffectObject.remove(QStringLiteral("bevelHighlightOpacity"));
    legacyEffectObject.remove(QStringLiteral("bevelShadowColour"));
    legacyEffectObject.remove(QStringLiteral("bevelShadowBlendMode"));
    legacyEffectObject.remove(QStringLiteral("bevelShadowOpacity"));
    bool legacyEffectOk = false;
    const LayerEffect migratedDefinition = LayerEffect::fromJson(
        legacyEffectObject, &legacyEffectOk);
    QVERIFY(legacyEffectOk);
    QCOMPARE(migratedDefinition.schema, LayerEffect::CurrentSchema);
    QCOMPARE(migratedDefinition.type, LayerEffectType::DropShadow);
    QVERIFY(!migratedDefinition.enabled);
    QCOMPARE(migratedDefinition.distance, 10.0);
    QCOMPARE(migratedDefinition.size, 10.0);

    // 0.14.0i schema-2 future Stroke definitions used zero-size placeholder
    // fields. They migrate disabled with usable 0.14.0j defaults rather than
    // becoming a silently invisible zero-width Stroke when later enabled.
    LayerEffect schema2Stroke;
    schema2Stroke.type = LayerEffectType::Stroke;
    schema2Stroke.enabled = false;
    schema2Stroke.size = 0.0;
    schema2Stroke.normalise();
    QJsonObject schema2StrokeObject = schema2Stroke.toJson();
    schema2StrokeObject.insert(QStringLiteral("schema"), 2);
    schema2StrokeObject.remove(QStringLiteral("strokePosition"));
    schema2StrokeObject.remove(QStringLiteral("gradientStops"));
    schema2StrokeObject.remove(QStringLiteral("gradientInterpolation"));
    schema2StrokeObject.remove(QStringLiteral("gradientStyle"));
    schema2StrokeObject.remove(QStringLiteral("gradientAngle"));
    schema2StrokeObject.remove(QStringLiteral("gradientScale"));
    schema2StrokeObject.remove(QStringLiteral("gradientReverse"));
    schema2StrokeObject.remove(QStringLiteral("bevelStyle"));
    schema2StrokeObject.remove(QStringLiteral("bevelDirection"));
    schema2StrokeObject.remove(QStringLiteral("bevelDepth"));
    schema2StrokeObject.remove(QStringLiteral("bevelSoften"));
    schema2StrokeObject.remove(QStringLiteral("bevelAltitude"));
    schema2StrokeObject.remove(QStringLiteral("bevelHighlightColour"));
    schema2StrokeObject.remove(QStringLiteral("bevelHighlightBlendMode"));
    schema2StrokeObject.remove(QStringLiteral("bevelHighlightOpacity"));
    schema2StrokeObject.remove(QStringLiteral("bevelShadowColour"));
    schema2StrokeObject.remove(QStringLiteral("bevelShadowBlendMode"));
    schema2StrokeObject.remove(QStringLiteral("bevelShadowOpacity"));
    bool schema2StrokeOk = false;
    const LayerEffect migratedStroke = LayerEffect::fromJson(
        schema2StrokeObject, &schema2StrokeOk);
    QVERIFY(schema2StrokeOk);
    QVERIFY(!migratedStroke.enabled);
    QCOMPARE(migratedStroke.size, 3.0);
    QCOMPARE(migratedStroke.strokePosition, LayerEffectStrokePosition::Outside);

    LayerEffect schema3Bevel;
    schema3Bevel.type = LayerEffectType::BevelEmboss;
    schema3Bevel.enabled = false;
    schema3Bevel.normalise();
    QJsonObject schema3BevelObject = schema3Bevel.toJson();
    schema3BevelObject.insert(QStringLiteral("schema"), 3);
    for (const QString &field : {QStringLiteral("bevelStyle"), QStringLiteral("bevelDirection"),
                                 QStringLiteral("bevelDepth"), QStringLiteral("bevelSoften"),
                                 QStringLiteral("bevelAltitude"), QStringLiteral("bevelHighlightColour"),
                                 QStringLiteral("bevelHighlightBlendMode"), QStringLiteral("bevelHighlightOpacity"),
                                 QStringLiteral("bevelShadowColour"), QStringLiteral("bevelShadowBlendMode"),
                                 QStringLiteral("bevelShadowOpacity")}) {
        schema3BevelObject.remove(field);
    }
    bool schema3BevelOk = false;
    const LayerEffect migratedBevel = LayerEffect::fromJson(schema3BevelObject, &schema3BevelOk);
    QVERIFY(schema3BevelOk);
    QVERIFY(!migratedBevel.enabled);
    QCOMPARE(migratedBevel.size, 8.0);
    QCOMPARE(migratedBevel.bevelStyle, LayerEffectBevelStyle::InnerBevel);
    QCOMPARE(migratedBevel.bevelDepth, 100.0);
    QCOMPARE(migratedBevel.bevelHighlightBlendMode, BlendMode::Screen);
    QCOMPARE(migratedBevel.bevelShadowBlendMode, BlendMode::Multiply);

    QVERIFY(document.moveLayerEffect(rasterId, strokeId, 0));
    configured = document.layerById(rasterId);
    QCOMPARE(configured.layerEffects.at(0).id, strokeId);
    QCOMPARE(configured.layerEffects.at(1).id, shadowId);
    const QImage beforeSmartConversion = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!beforeSmartConversion.isNull());

    // Conversion must preserve the actual Layer Effect definitions inside the
    // authoritative embedded layer tree rather than flattening them away.
    const QUuid smartId = document.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    const QImage afterSmartConversion = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, document.sourceImage().size(),
        document.colourState().processingCompatibility);
    QVERIFY(!afterSmartConversion.isNull());
    QVERIFY(exactImagesEqual(beforeSmartConversion, afterSmartConversion));
    const QUuid sourceId = document.layerById(smartId).smartSource.sourceId;
    const SmartSourceDescriptor *source = document.smartSources().find(sourceId);
    QVERIFY(source != nullptr);
    QCOMPARE(source->embeddedDocument.value(QStringLiteral("schema")).toInt(), 10);
    QVector<LayerNode> embeddedLayers;
    QVERIFY2(document.embeddedSmartSourceLayers(sourceId, &embeddedLayers,
                                                 nullptr, nullptr, &error),
             qPrintable(error));
    QCOMPARE(embeddedLayers.size(), 1);
    QCOMPARE(embeddedLayers.constFirst().layerEffects.size(), 8);
    QCOMPARE(embeddedLayers.constFirst().layerEffects.at(0).id, strokeId);

    const QUuid overlayId = document.addLayerEffect(
        smartId, LayerEffectType::ColourOverlay, &error);
    QVERIFY2(!overlayId.isNull(), qPrintable(error));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("layer-effects-v26.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument encoded = QJsonDocument::fromJson(file.readAll());
    QVERIFY(encoded.isObject());
    QCOMPARE(encoded.object().value(QStringLiteral("version")).toInt(), 27);

    PhotoDocument reopened;
    QVERIFY2(reopened.loadProject(path, &error), qPrintable(error));
    QCOMPARE(reopened.layerById(smartId).layerEffects.size(), 1);
    QCOMPARE(reopened.layerById(smartId).layerEffects.constFirst().id, overlayId);
    QVector<LayerNode> reopenedEmbedded;
    QVERIFY2(reopened.embeddedSmartSourceLayers(sourceId, &reopenedEmbedded,
                                                 nullptr, nullptr, &error),
             qPrintable(error));
    QCOMPARE(reopenedEmbedded.constFirst().layerEffects.size(), 8);
    const auto reopenedShadowIt = std::find_if(
        reopenedEmbedded.constFirst().layerEffects.cbegin(),
        reopenedEmbedded.constFirst().layerEffects.cend(),
        [shadowId](const LayerEffect &effect) { return effect.id == shadowId; });
    QVERIFY(reopenedShadowIt != reopenedEmbedded.constFirst().layerEffects.cend());
    QCOMPARE(reopenedShadowIt->effectOpacity, 1.0);
    QCOMPARE(reopenedShadowIt->effectBlendMode, BlendMode::Multiply);
    QCOMPARE(reopenedShadowIt->angleDegrees, 180.0);
    QCOMPARE(reopenedShadowIt->distance, 8.0);
    QCOMPARE(reopenedShadowIt->size, 0.0);
    const auto reopenedGradientIt = std::find_if(
        reopenedEmbedded.constFirst().layerEffects.cbegin(),
        reopenedEmbedded.constFirst().layerEffects.cend(),
        [gradientOverlayId](const LayerEffect &effect) {
            return effect.id == gradientOverlayId;
        });
    QVERIFY(reopenedGradientIt != reopenedEmbedded.constFirst().layerEffects.cend());
    QCOMPARE(reopenedGradientIt->schema, LayerEffect::CurrentSchema);
    QCOMPARE(reopenedGradientIt->gradientStyle, LayerEffectGradientStyle::Linear);
    QCOMPARE(reopenedGradientIt->gradientInterpolation, GradientInterpolation::Linear);
    QCOMPARE(reopenedGradientIt->gradientAngleDegrees, 0.0);
    QCOMPARE(reopenedGradientIt->gradientScale, 100.0);
    QCOMPARE(reopenedGradientIt->gradientReverse, false);
    QCOMPARE(reopenedGradientIt->gradientStops.size(), 3);
    QCOMPARE(reopenedGradientIt->gradientStops.at(1).colour, QColor(0, 255, 0));
    const auto reopenedBevelIt = std::find_if(
        reopenedEmbedded.constFirst().layerEffects.cbegin(),
        reopenedEmbedded.constFirst().layerEffects.cend(),
        [bevelId](const LayerEffect &effect) { return effect.id == bevelId; });
    QVERIFY(reopenedBevelIt != reopenedEmbedded.constFirst().layerEffects.cend());
    QCOMPARE(reopenedBevelIt->schema, LayerEffect::CurrentSchema);
    QCOMPARE(reopenedBevelIt->bevelStyle, LayerEffectBevelStyle::InnerBevel);
    QCOMPARE(reopenedBevelIt->bevelDirection, LayerEffectBevelDirection::Up);
    QCOMPARE(reopenedBevelIt->bevelDepth, 180.0);
    QCOMPARE(reopenedBevelIt->bevelSoften, 1.5);
    QCOMPARE(reopenedBevelIt->bevelAltitudeDegrees, 30.0);
    QCOMPARE(reopenedBevelIt->bevelHighlightBlendMode, BlendMode::Screen);
    QCOMPARE(reopenedBevelIt->bevelShadowBlendMode, BlendMode::Multiply);

    // A project claiming to predate 0.14.0k Bevel & Emboss parameters must not be able to smuggle
    // embedded schema-9 fx parameters through a Smart Source. Remove the root
    // Smart instance's fx first so this specifically exercises the embedded gate.
    QJsonObject forgedRoot = encoded.object();
    forgedRoot.insert(QStringLiteral("version"), 25);
    QJsonArray forgedTree = forgedRoot.value(QStringLiteral("layerTree")).toArray();
    QVERIFY(mutateLayerObject(&forgedTree, smartId, [](QJsonObject &object) {
        object.remove(QStringLiteral("layerEffects"));
    }));
    forgedRoot.insert(QStringLiteral("layerTree"), forgedTree);
    const QString forgedPath = directory.filePath(
        QStringLiteral("forged-v25-embedded-bevel-parameters.vfxphoto"));
    QFile forgedFile(forgedPath);
    QVERIFY(forgedFile.open(QIODevice::WriteOnly));
    QVERIFY(forgedFile.write(QJsonDocument(forgedRoot).toJson(QJsonDocument::Compact)) > 0);
    forgedFile.close();
    PhotoDocument forged;
    QString forgedError;
    QVERIFY(!forged.loadProject(forgedPath, &forgedError));
    QVERIFY(forgedError.contains(QStringLiteral("Layer Effect"), Qt::CaseInsensitive));
}


void CoreTests::smartLayerTiledCacheReusesIntermediateTilesAndInvalidatesSelectively()
{
    SmartLayerTileCache &cache = SmartLayerTileCache::instance();
    cache.clear();
    const qsizetype previousBudget = cache.ramBudget();
    cache.setRamBudget(qsizetype(24) * 1024 * 1024);

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Smart Tile Cache Reuse");
    settings.pixelSize = QSize(512, 320);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid backgroundId = document.addRasterLayer();
    QVERIFY(!backgroundId.isNull());
    QImage background(settings.pixelSize, QImage::Format_RGBA8888);
    background.fill(QColor(32, 44, 58, 255));
    background.setColorSpace(document.sourceImage().colorSpace());
    QVERIFY(document.updateLayer(backgroundId, [&](LayerNode &layer) {
        layer.rasterImage = background;
    }));

    const QUuid sourceLayerId = document.addRasterLayer();
    QVERIFY(!sourceLayerId.isNull());
    QImage sourcePixels(QSize(420, 260), QImage::Format_RGBA8888);
    sourcePixels.fill(Qt::transparent);
    sourcePixels.setColorSpace(document.sourceImage().colorSpace());
    for (int y = 0; y < sourcePixels.height(); ++y) {
        uchar *row = sourcePixels.scanLine(y);
        for (int x = 0; x < sourcePixels.width(); ++x) {
            row[x * 4] = static_cast<uchar>((x * 11 + y * 3) & 255);
            row[x * 4 + 1] = static_cast<uchar>((x * 5 + y * 13) & 255);
            row[x * 4 + 2] = static_cast<uchar>((x * 17 + y * 7) & 255);
            row[x * 4 + 3] = static_cast<uchar>(80 + ((x + y) % 176));
        }
    }
    QVERIFY(document.updateLayer(sourceLayerId, [&](LayerNode &layer) {
        layer.rasterImage = sourcePixels;
        layer.rasterReferenceSize = sourcePixels.size();
    }));

    const QUuid smartId = document.convertLayersToEmbeddedSmart({sourceLayerId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    QImage instanceMask(settings.pixelSize, QImage::Format_Grayscale8);
    for (int y = 0; y < instanceMask.height(); ++y) {
        uchar *row = instanceMask.scanLine(y);
        for (int x = 0; x < instanceMask.width(); ++x) {
            row[x] = static_cast<uchar>(48 + ((x * 3 + y * 5) % 208));
        }
    }
    QVERIFY(document.updateLayer(smartId, [&](LayerNode &layer) {
        layer.maskImage = instanceMask;
        layer.maskReferenceSize = settings.pixelSize;
        layer.maskReferenceOrigin = QPointF(0.0, 0.0);
        layer.maskEnabled = true;
        layer.maskInverted = false;
        layer.transform = QTransform::fromTranslate(31.0, 19.0)
            * QTransform::fromScale(0.83, 0.79);
        layer.smartTransform.interpolation = TransformInterpolation::Lanczos3;
    }));
    const LayerNode smart = document.layerById(smartId);
    const QUuid sourceId = smart.smartSource.sourceId;
    QVERIFY(!sourceId.isNull());

    const QRect region(0, 0, 256, 256);
    const QImage first = ImageProcessor::renderRegion(
        document.sourceImage(), document.layers(), region, document.sourceImage().size(),
        nullptr, document.colourState().processingCompatibility);
    QVERIFY(!first.isNull());
    const SmartLayerTileCache::Stats afterFirst = cache.stats();
    QVERIFY(afterFirst.transformedMisses >= 1);
    QVERIFY(afterFirst.sourceMisses >= 1);
    QVERIFY(afterFirst.ramBytes > 0);
    QVERIFY(afterFirst.ramBytes <= cache.ramBudget());

    // Editing only a layer underneath invalidates the final composite, not the
    // Smart intermediate. The second render must hit the transformed cache.
    QImage changedBackground = background;
    changedBackground.detach();
    QPainter backgroundPainter(&changedBackground);
    backgroundPainter.fillRect(QRect(12, 15, 90, 70), QColor(190, 30, 80, 255));
    backgroundPainter.end();
    QVERIFY(document.updateLayer(backgroundId, [&](LayerNode &layer) {
        layer.rasterImage = changedBackground;
    }));
    const QImage second = ImageProcessor::renderRegion(
        document.sourceImage(), document.layers(), region, document.sourceImage().size(),
        nullptr, document.colourState().processingCompatibility);
    QVERIFY(!second.isNull());
    QVERIFY(!exactImagesEqual(first, second));
    const SmartLayerTileCache::Stats afterLowerEdit = cache.stats();
    // Both the transformed Smart content and the transformed instance mask
    // are reusable intermediates when only a lower layer changes.
    QVERIFY(afterLowerEdit.transformedHits >= afterFirst.transformedHits + 2);
    QCOMPARE(afterLowerEdit.transformedMisses, afterFirst.transformedMisses);

    // A far Edit Contents change advances the authoritative source revision but
    // does not touch this output tile's inverse-mapped source footprint. The
    // content-addressed transformed intermediate should therefore survive the
    // revision bump rather than being flushed by source identity alone.
    PhotoDocument farEditor;
    QHash<QUuid, quint64> farBaseline;
    QVERIFY2(document.createEditableSmartSourceDocument(sourceId, &farEditor,
                                                         &farBaseline, &error),
             qPrintable(error));
    QImage farPixels = sourcePixels;
    farPixels.detach();
    QPainter farPainter(&farPixels);
    farPainter.fillRect(QRect(365, 185, 40, 45), QColor(240, 45, 75, 210));
    farPainter.end();
    QVERIFY(farEditor.updateLayer(sourceLayerId, [&](LayerNode &layer) {
        layer.rasterImage = farPixels;
    }));
    QVERIFY2(document.commitEditableSmartSourceDocument(sourceId, farEditor, farBaseline,
                                                          nullptr, &error),
             qPrintable(error));
    const SmartLayerTileCache::Stats afterFarCommit = cache.stats();
    const QImage afterFarEdit = ImageProcessor::renderRegion(
        document.sourceImage(), document.layers(), region, document.sourceImage().size(),
        nullptr, document.colourState().processingCompatibility);
    QVERIFY(!afterFarEdit.isNull());
    QVERIFY(exactImagesEqual(second, afterFarEdit));
    const SmartLayerTileCache::Stats afterFarRender = cache.stats();
    QVERIFY(afterFarRender.transformedHits >= afterFarCommit.transformedHits + 2);
    QCOMPARE(afterFarRender.transformedMisses, afterFarCommit.transformedMisses);

    // Editing inside this output tile's Smart dependency footprint changes the
    // source-region fingerprint, so the content intermediate must be rebuilt.
    PhotoDocument nearEditor;
    QHash<QUuid, quint64> nearBaseline;
    QVERIFY2(document.createEditableSmartSourceDocument(sourceId, &nearEditor,
                                                         &nearBaseline, &error),
             qPrintable(error));
    QImage editedPixels = farPixels;
    editedPixels.detach();
    QPainter sourcePainter(&editedPixels);
    sourcePainter.fillRect(QRect(140, 90, 80, 60), QColor(25, 240, 100, 210));
    sourcePainter.end();
    QVERIFY(nearEditor.updateLayer(sourceLayerId, [&](LayerNode &layer) {
        layer.rasterImage = editedPixels;
    }));
    QVERIFY2(document.commitEditableSmartSourceDocument(sourceId, nearEditor, nearBaseline,
                                                          nullptr, &error),
             qPrintable(error));
    const SmartLayerTileCache::Stats afterNearCommit = cache.stats();

    const QImage third = ImageProcessor::renderRegion(
        document.sourceImage(), document.layers(), region, document.sourceImage().size(),
        nullptr, document.colourState().processingCompatibility);
    QVERIFY(!third.isNull());
    QVERIFY(!exactImagesEqual(afterFarEdit, third));
    const SmartLayerTileCache::Stats afterSourceEdit = cache.stats();
    QVERIFY(afterSourceEdit.transformedMisses > afterNearCommit.transformedMisses);
    QVERIFY(afterSourceEdit.ramBytes <= cache.ramBudget());

    const quint64 evictionsBeforeClamp = afterSourceEdit.evictions;
    cache.setRamBudget(qsizetype(512) * 1024);
    const SmartLayerTileCache::Stats afterClamp = cache.stats();
    QVERIFY(afterClamp.ramBytes <= cache.ramBudget());
    QVERIFY(afterClamp.evictions > evictionsBeforeClamp);

    cache.clear();
    cache.setRamBudget(previousBudget);
}

void CoreTests::smartLayerCompositeFingerprintIsTileLocal()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Smart Tile Local Fingerprint");
    settings.pixelSize = QSize(768, 256);
    settings.backgroundColour = QColor(18, 22, 30, 255);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const QUuid sourceLayerId = document.addRasterLayer();
    QImage pixels(QSize(96, 96), QImage::Format_RGBA8888);
    pixels.fill(QColor(230, 80, 40, 255));
    pixels.setColorSpace(document.sourceImage().colorSpace());
    QVERIFY(document.updateLayer(sourceLayerId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = pixels.size();
    }));
    const QUuid smartId = document.convertLayersToEmbeddedSmart({sourceLayerId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    QVERIFY(document.updateLayer(smartId, [](LayerNode &layer) {
        layer.transform = QTransform::fromTranslate(430.0, 50.0);
    }));

    TiledCanvasEngine engine(nullptr);
    const QUuid sessionId = QUuid::createUuid();
    const QRect leftTile(0, 0, 256, 256);
    const QImage first = engine.renderRegion(document.previewSource(), document.layers(),
                                             leftTile, document.sourceImage().size(),
                                             false, 0, nullptr, nullptr, sessionId,
                                             document.colourStateRevision(),
                                             document.colourState().processingCompatibility);
    QVERIFY(!first.isNull());
    const TileCache::Stats firstStats = engine.cacheStatsForSession(sessionId);

    // Both transforms leave the Smart Layer entirely outside the requested
    // left tile. Tile-local contribution hashing must therefore preserve the
    // already-rendered left composite tile.
    QVERIFY(document.updateLayer(smartId, [](LayerNode &layer) {
        QTransform rotation;
        rotation.rotate(13.0);
        layer.transform = QTransform::fromTranslate(560.0, 70.0) * rotation;
    }));
    const QImage second = engine.renderRegion(document.previewSource(), document.layers(),
                                              leftTile, document.sourceImage().size(),
                                              false, 0, nullptr, nullptr, sessionId,
                                              document.colourStateRevision(),
                                              document.colourState().processingCompatibility);
    QVERIFY(!second.isNull());
    const TileCache::Stats secondStats = engine.cacheStatsForSession(sessionId);
    QVERIFY(secondStats.hits > firstStats.hits);
    QCOMPARE(secondStats.misses, firstStats.misses);
    QVERIFY(exactImagesEqual(first, second));
}


void CoreTests::smartLayerSourceDirtyPropagationKeepsUnaffectedCompositeTiles()
{
    SmartLayerTileCache::instance().clear();
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Smart Source Dirty Propagation");
    settings.pixelSize = QSize(768, 256);
    settings.backgroundColour = QColor(15, 18, 24, 255);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const QUuid sourceLayerId = document.addRasterLayer();
    QImage pixels(QSize(512, 256), QImage::Format_RGBA8888);
    pixels.fill(QColor(60, 110, 180, 255));
    pixels.setColorSpace(document.sourceImage().colorSpace());
    QVERIFY(document.updateLayer(sourceLayerId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = pixels.size();
    }));
    const QUuid smartId = document.convertLayersToEmbeddedSmart({sourceLayerId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    const QUuid sourceId = document.layerById(smartId).smartSource.sourceId;
    QVERIFY(!sourceId.isNull());

    TiledCanvasEngine engine(nullptr);
    const QUuid sessionId = QUuid::createUuid();
    const QRect leftTile(0, 0, 256, 256);
    const auto renderLeft = [&]() {
        return engine.renderRegion(document.previewSource(), document.layers(),
                                   leftTile, document.sourceImage().size(), false,
                                   0, nullptr, nullptr, sessionId,
                                   document.colourStateRevision(),
                                   document.colourState().processingCompatibility);
    };
    const QImage before = renderLeft();
    QVERIFY(!before.isNull());
    const TileCache::Stats beforeStats = engine.cacheStatsForSession(sessionId);

    // Edit source pixels far outside the inverse-mapped source footprint of the
    // left output tile. The Smart Source revision advances, but exact source
    // region fingerprints keep the unchanged left composite tile reusable.
    PhotoDocument farEditor;
    QHash<QUuid, quint64> farBaseline;
    QVERIFY2(document.createEditableSmartSourceDocument(sourceId, &farEditor,
                                                         &farBaseline, &error),
             qPrintable(error));
    QImage farPixels = pixels;
    farPixels.detach();
    QPainter farPainter(&farPixels);
    farPainter.fillRect(QRect(420, 70, 45, 50), QColor(240, 45, 75, 255));
    farPainter.end();
    QVERIFY(farEditor.updateLayer(sourceLayerId, [&](LayerNode &layer) {
        layer.rasterImage = farPixels;
    }));
    QVERIFY2(document.commitEditableSmartSourceDocument(sourceId, farEditor,
                                                          farBaseline, nullptr, &error),
             qPrintable(error));
    const QImage afterFarEdit = renderLeft();
    QVERIFY(!afterFarEdit.isNull());
    QVERIFY(exactImagesEqual(before, afterFarEdit));
    const TileCache::Stats farStats = engine.cacheStatsForSession(sessionId);
    QVERIFY(farStats.hits > beforeStats.hits);
    QCOMPARE(farStats.misses, beforeStats.misses);

    // Now edit inside the left tile's Smart source dependency. Its source
    // fingerprint must change and the parent composite tile must be regenerated.
    PhotoDocument nearEditor;
    QHash<QUuid, quint64> nearBaseline;
    QVERIFY2(document.createEditableSmartSourceDocument(sourceId, &nearEditor,
                                                         &nearBaseline, &error),
             qPrintable(error));
    QImage nearPixels = farPixels;
    nearPixels.detach();
    QPainter nearPainter(&nearPixels);
    nearPainter.fillRect(QRect(70, 60, 55, 45), QColor(35, 235, 90, 255));
    nearPainter.end();
    QVERIFY(nearEditor.updateLayer(sourceLayerId, [&](LayerNode &layer) {
        layer.rasterImage = nearPixels;
    }));
    QVERIFY2(document.commitEditableSmartSourceDocument(sourceId, nearEditor,
                                                          nearBaseline, nullptr, &error),
             qPrintable(error));
    const QImage afterNearEdit = renderLeft();
    QVERIFY(!afterNearEdit.isNull());
    QVERIFY(!exactImagesEqual(afterFarEdit, afterNearEdit));
    const TileCache::Stats nearStats = engine.cacheStatsForSession(sessionId);
    QVERIFY(nearStats.misses > farStats.misses);

    // Gradient Overlay is intentionally different from local Smart dirty
    // propagation: its mapping is anchored to the owner's complete effective
    // coverage bounds. Expanding a mask only on the distant half leaves the
    // left tile's local coverage identical but moves the gradient span, so the
    // left composite must be invalidated rather than reused stale.
    QImage narrowMask(pixels.size(), QImage::Format_Grayscale8);
    narrowMask.fill(0);
    QPainter narrowMaskPainter(&narrowMask);
    narrowMaskPainter.fillRect(QRect(0, 0, 256, pixels.height()), Qt::white);
    narrowMaskPainter.end();
    QVERIFY(document.updateLayer(smartId, [&](LayerNode &layer) {
        layer.maskImage = narrowMask;
        layer.maskReferenceSize = pixels.size();
        layer.maskReferenceOrigin = {};
        layer.maskEnabled = true;
        layer.maskInverted = false;
    }));
    const QUuid gradientId = document.addLayerEffect(
        smartId, LayerEffectType::GradientOverlay, &error);
    QVERIFY2(!gradientId.isNull(), qPrintable(error));
    LayerNode gradientOwner = document.layerById(smartId);
    const auto gradientEffectIt = std::find_if(
        gradientOwner.layerEffects.cbegin(), gradientOwner.layerEffects.cend(),
        [gradientId](const LayerEffect &effect) { return effect.id == gradientId; });
    QVERIFY(gradientEffectIt != gradientOwner.layerEffects.cend());
    LayerEffect gradientEffect = *gradientEffectIt;
    gradientEffect.gradientStops = {{0.0, QColor(255, 30, 30)},
                                    {1.0, QColor(20, 40, 255)}};
    gradientEffect.gradientStyle = LayerEffectGradientStyle::Linear;
    gradientEffect.gradientAngleDegrees = 0.0;
    gradientEffect.gradientScale = 100.0;
    gradientEffect.effectOpacity = 1.0;
    gradientEffect.normalise();
    QVERIFY(document.updateLayerEffect(smartId, gradientId, gradientEffect, &error));
    const QImage narrowGradient = renderLeft();
    QVERIFY(!narrowGradient.isNull());
    const TileCache::Stats narrowGradientStats = engine.cacheStatsForSession(sessionId);

    QImage wideMask = narrowMask;
    wideMask.detach();
    QPainter wideMaskPainter(&wideMask);
    wideMaskPainter.fillRect(QRect(256, 0, 256, pixels.height()), Qt::white);
    wideMaskPainter.end();
    QVERIFY(document.updateLayer(smartId, [&](LayerNode &layer) {
        layer.maskImage = wideMask;
    }));
    const QImage wideGradient = renderLeft();
    QVERIFY(!wideGradient.isNull());
    const TileCache::Stats wideGradientStats = engine.cacheStatsForSession(sessionId);
    QVERIFY(wideGradientStats.misses > narrowGradientStats.misses);
    QVERIFY(!exactImagesEqual(narrowGradient, wideGradient));

    SmartLayerTileCache::instance().clear();
}

void CoreTests::smartLayerColdEvictionPurgesRuntimeIntermediates()
{
    SmartLayerTileCache &cache = SmartLayerTileCache::instance();
    cache.clear();
    const qsizetype previousBudget = cache.ramBudget();
    cache.setRamBudget(qsizetype(8) * 1024 * 1024);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    SessionCacheStore store(temporaryDirectory.path());
    QVERIFY(store.isAvailable());

    DocumentSession session;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Smart Cold Cache Purge");
    settings.pixelSize = QSize(320, 256);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(session.document().createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(session.document().updateLayer(session.document().baseLayerId(),
                                             [](LayerNode &layer) { layer.visible = false; }));

    const QUuid rasterId = session.document().addRasterLayer();
    QImage pixels(settings.pixelSize, QImage::Format_RGBA8888);
    pixels.fill(Qt::transparent);
    pixels.setColorSpace(session.document().sourceImage().colorSpace());
    for (int y = 0; y < pixels.height(); ++y) {
        uchar *row = pixels.scanLine(y);
        for (int x = 0; x < pixels.width(); ++x) {
            row[x * 4] = static_cast<uchar>((x * 9 + y * 5) & 255);
            row[x * 4 + 1] = static_cast<uchar>((x * 3 + y * 11) & 255);
            row[x * 4 + 2] = static_cast<uchar>((x * 7 + y * 13) & 255);
            row[x * 4 + 3] = static_cast<uchar>(120 + ((x + y) % 136));
        }
    }
    QVERIFY(session.document().updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = pixels.size();
    }));
    const QUuid smartId = session.document().convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    QVERIFY(session.document().updateLayer(smartId, [](LayerNode &layer) {
        QTransform rotation;
        rotation.rotate(7.0);
        layer.transform = QTransform::fromScale(0.91, 0.87) * rotation;
        layer.smartTransform.interpolation = TransformInterpolation::Lanczos3;
    }));

    const QImage rendered = ImageProcessor::renderRegion(
        session.document().sourceImage(), session.document().layers(),
        QRect(0, 0, 256, 256), session.document().sourceImage().size(), nullptr,
        session.document().colourState().processingCompatibility);
    QVERIFY(!rendered.isNull());
    const SmartLayerTileCache::Stats populated = cache.stats();
    QVERIFY(populated.ramBytes > 0);
    QVERIFY(populated.sourceTiles > 0 || populated.transformedTiles > 0);

    QVERIFY2(session.evictToDisk(store, &error), qPrintable(error));
    QCOMPARE(session.residency(), SessionResidency::Cold);
    const SmartLayerTileCache::Stats cold = cache.stats();
    QCOMPARE(cold.ramBytes, qsizetype(0));
    QCOMPARE(cold.sourceTiles, 0);
    QCOMPARE(cold.transformedTiles, 0);

    QVERIFY2(session.restoreFromDisk(store, &error), qPrintable(error));
    QVERIFY(session.document().hasImage());
    const QImage restoredRender = ImageProcessor::renderRegion(
        session.document().sourceImage(), session.document().layers(),
        QRect(0, 0, 256, 256), session.document().sourceImage().size(), nullptr,
        session.document().colourState().processingCompatibility);
    QVERIFY(!restoredRender.isNull());
    const SmartLayerTileCache::Stats repopulated = cache.stats();
    QVERIFY(repopulated.ramBytes > 0);

    cache.clear();
    cache.setRamBudget(previousBudget);
}

void CoreTests::embeddedSmartSourceSurvivesSessionSnapshot()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    SessionCacheStore store(temporaryDirectory.path());
    QVERIFY(store.isAvailable());

    DocumentSession original;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Embedded Smart Cold Storage");
    settings.pixelSize = QSize(28, 22);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(original.document().createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(original.document().updateLayer(original.document().baseLayerId(),
                                             [](LayerNode &layer) { layer.visible = false; }));

    const QUuid rasterId = original.document().addRasterLayer();
    QVERIFY(!rasterId.isNull());
    QImage pixels(settings.pixelSize, QImage::Format_RGBA8888);
    pixels.fill(Qt::transparent);
    for (int y = 4; y < 18; ++y) {
        uchar *row = pixels.scanLine(y);
        for (int x = 5; x < 23; ++x) {
            const int offset = x * 4;
            row[offset] = static_cast<uchar>(30 + x * 5);
            row[offset + 1] = static_cast<uchar>(170 - y * 3);
            row[offset + 2] = 210;
            row[offset + 3] = static_cast<uchar>(70 + ((x + y) * 7) % 180);
        }
    }
    pixels.scanLine(1)[8] = 173;
    pixels.scanLine(1)[9] = 41;
    pixels.scanLine(1)[10] = 229;
    pixels.scanLine(1)[11] = 0;
    QVERIFY(original.document().updateLayer(rasterId, [&](LayerNode &layer) {
        layer.name = QStringLiteral("Embedded Source Raster");
        layer.rasterImage = pixels;
        layer.transform.translate(-2.0, 1.0);
    }));

    const QUuid smartId = original.document().convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    const LayerNode originalSmart = original.document().layerById(smartId);
    const SmartSourceDescriptor *originalSource = original.document().smartSources().find(
        originalSmart.smartSource.sourceId);
    QVERIFY(originalSource != nullptr);
    QVERIFY(originalSource->hasEmbeddedDocument());
    QVERIFY(originalSource->hasCurrentPresentation());
    const QImage expected = ImageProcessor::renderPreservingHiddenRgb(
        original.document().sourceImage(), original.document().layers(), nullptr,
        original.document().sourceImage().size(),
        original.document().colourState().processingCompatibility);
    QVERIFY(!expected.isNull());

    QString snapshotPath;
    QVERIFY2(store.writeSnapshot(original, &snapshotPath, nullptr, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(snapshotPath));

    DocumentSession restored;
    QVERIFY2(store.restoreSnapshot(snapshotPath, &restored, &error), qPrintable(error));
    const LayerNode restoredSmart = restored.document().layerById(smartId);
    QCOMPARE(restoredSmart.type, LayerType::Smart);
    QVERIFY(!restoredSmart.smartPresentationImage.isNull());
    const SmartSourceDescriptor *restoredSource = restored.document().smartSources().find(
        restoredSmart.smartSource.sourceId);
    QVERIFY(restoredSource != nullptr);
    QVERIFY(restoredSource->hasEmbeddedDocument());
    QVERIFY(restoredSource->hasCurrentPresentation());
    QVector<LayerNode> embedded;
    QVERIFY2(restored.document().embeddedSmartSourceLayers(restoredSource->id, &embedded,
                                                            nullptr, nullptr, &error),
             qPrintable(error));
    QCOMPARE(embedded.size(), 1);
    QCOMPARE(embedded.constFirst().id, rasterId);
    const QImage restoredRender = ImageProcessor::renderPreservingHiddenRgb(
        restored.document().sourceImage(), restored.document().layers(), nullptr,
        restored.document().sourceImage().size(),
        restored.document().colourState().processingCompatibility);
    QVERIFY(imagesWithinChannelTolerance(expected, restoredRender, 1));

    QFile snapshotFile(snapshotPath);
    QVERIFY(snapshotFile.open(QIODevice::ReadOnly));
    QByteArray legacyEnvelope = snapshotFile.readAll();
    snapshotFile.close();
    QVERIFY(legacyEnvelope.size() > 12);
    legacyEnvelope[8] = char(18);
    legacyEnvelope[9] = char(0);
    legacyEnvelope[10] = char(0);
    legacyEnvelope[11] = char(0);
    const QString dishonestPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-embedded-smart-session.bin"));
    QFile dishonestFile(dishonestPath);
    QVERIFY(dishonestFile.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestFile.write(legacyEnvelope),
             static_cast<qint64>(legacyEnvelope.size()));
    dishonestFile.close();
    DocumentSession rejected;
    QString rejectedError;
    QVERIFY(!store.restoreSnapshot(dishonestPath, &rejected, &rejectedError));
    QVERIFY(!rejectedError.isEmpty());

    QByteArray prePrecisionEnvelope = legacyEnvelope;
    prePrecisionEnvelope[8] = char(19);
    const QString dishonestPrecisionPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-precision-smart-session.bin"));
    QFile dishonestPrecisionFile(dishonestPrecisionPath);
    QVERIFY(dishonestPrecisionFile.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPrecisionFile.write(prePrecisionEnvelope),
             static_cast<qint64>(prePrecisionEnvelope.size()));
    dishonestPrecisionFile.close();
    DocumentSession rejectedPrecision;
    QString precisionError;
    QVERIFY(!store.restoreSnapshot(dishonestPrecisionPath,
                                   &rejectedPrecision, &precisionError));
    QVERIFY(!precisionError.isEmpty());
}

void CoreTests::smartSourceEditorBindingSurvivesSessionSnapshot()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    SessionCacheStore store(temporaryDirectory.path());
    QVERIFY(store.isAvailable());

    PhotoDocument owner;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Source Editor Snapshot");
    settings.pixelSize = QSize(24, 18);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(owner.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(owner.updateLayer(owner.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));
    const QUuid rasterId = owner.addRasterLayer();
    QVERIFY(!rasterId.isNull());
    QImage pixels(settings.pixelSize, QImage::Format_RGBA8888);
    pixels.fill(QColor(45, 120, 210, 170));
    pixels.setColorSpace(owner.sourceImage().colorSpace());
    QVERIFY(owner.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
    }));
    const QUuid smartId = owner.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    const QUuid sourceId = owner.layerById(smartId).smartSource.sourceId;

    PhotoDocument editable;
    QHash<QUuid, quint64> baseline;
    QVERIFY2(owner.createEditableSmartSourceDocument(sourceId, &editable,
                                                       &baseline, &error),
             qPrintable(error));
    DocumentSession editor;
    editor.document() = std::move(editable);
    auto &binding = editor.smartSourceEditBinding();
    binding.ownerSessionId = QUuid::createUuid();
    binding.sourceId = sourceId;
    binding.sourceName = QStringLiteral("Nested Source");
    binding.baselineSourceRevisions = baseline;
    QVERIFY(editor.isSmartSourceEditor());

    QString snapshotPath;
    QVERIFY2(store.writeSnapshot(editor, &snapshotPath, nullptr, &error),
             qPrintable(error));
    QFile snapshot(snapshotPath);
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    QDataStream stream(&snapshot);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_6_0);
    char magic[8] = {};
    QCOMPARE(stream.readRawData(magic, sizeof(magic)), static_cast<int>(sizeof(magic)));
    quint32 version = 0;
    stream >> version;
    QCOMPARE(version, 25u);
    snapshot.close();

    DocumentSession restored;
    QVERIFY2(store.restoreSnapshot(snapshotPath, &restored, &error), qPrintable(error));
    QVERIFY(restored.isSmartSourceEditor());
    const auto &restoredBinding = restored.smartSourceEditBinding();
    QCOMPARE(restoredBinding.ownerSessionId, binding.ownerSessionId);
    QCOMPARE(restoredBinding.sourceId, binding.sourceId);
    QCOMPARE(restoredBinding.sourceName, binding.sourceName);
    QVERIFY(restoredBinding.baselineSourceRevisions
            == binding.baselineSourceRevisions);
    QVERIFY(restored.document().containsLayer(rasterId));
    QVERIFY(restored.document().smartSources().contains(sourceId));
}

void CoreTests::documentSessionsOwnIndependentState()
{
    DocumentSession first;
    DocumentSession second;

    QVERIFY(first.id() != second.id());
    QVERIFY(first.renderSerial() != second.renderSerial());
    QVERIFY(first.undoStack() != second.undoStack());
    QVERIFY(first.rasterHistoryStats() != second.rasterHistoryStats());

    NewDocumentSettings settings;
    settings.name = QStringLiteral("First Session");
    settings.pixelSize = QSize(32, 24);
    QString error;
    QVERIFY2(first.document().createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(!second.document().hasImage());

    first.editTargetLayerId() = first.document().baseLayerId();
    first.editTarget() = LayerEditTarget::Alpha;
    first.channelView() = ChannelView::Alpha;
    first.baselineRequiresSave() = true;
    first.selectedLayerIds() = {first.document().baseLayerId()};
    first.viewState().zoom = 2.5;
    first.viewState().scrollPosition = QPoint(41, 17);
    first.viewState().fitToView = false;
    first.viewState().valid = true;
    first.undoStack()->push(new QUndoCommand(QStringLiteral("First-only change")));
    first.rasterHistoryStats()->storedBytes = 128;

    QCOMPARE(first.undoStack()->count(), 1);
    QCOMPARE(second.undoStack()->count(), 0);
    QVERIFY(first.channelView() == ChannelView::Alpha);
    QVERIFY(second.channelView() == ChannelView::Composite);
    QVERIFY(first.baselineRequiresSave());
    QVERIFY(!second.baselineRequiresSave());
    QCOMPARE(first.selectedLayerIds().size(), 1);
    QVERIFY(second.selectedLayerIds().isEmpty());
    QCOMPARE(first.viewState().scrollPosition, QPoint(41, 17));
    QVERIFY(!first.viewState().fitToView);
    QCOMPARE(first.rasterHistoryStats()->storedBytes, qint64(128));
    QCOMPARE(second.rasterHistoryStats()->storedBytes, qint64(0));
}

void CoreTests::documentSessionResetKeepsDocumentButClearsTransientState()
{
    DocumentSession session;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(16, 16);
    QString error;
    QVERIFY2(session.document().createNewDocument(settings, &error), qPrintable(error));
    const QUuid baseId = session.document().baseLayerId();

    session.editTargetLayerId() = baseId;
    session.editTarget() = LayerEditTarget::Red;
    session.channelView() = ChannelView::Red;
    session.baselineRequiresSave() = true;
    session.propertyUndoActive() = true;
    session.propertyUndoText() = QStringLiteral("Exposure");
    session.selectedLayerIds() = {baseId};
    session.thumbnailCache().insert(QStringLiteral("base"), QIcon());
    session.viewState().zoom = 4.0;
    session.viewState().valid = true;
    session.undoStack()->push(new QUndoCommand(QStringLiteral("Change")));
    session.maskHistoryStats()->tileCount = 7;

    const quint64 beforeSerial = session.renderSerial();
    session.resetDocumentLocalState();

    // Replacing the contents of an existing session is a MainWindow concern;
    // the reset transaction deliberately keeps the current PhotoDocument alive.
    QVERIFY(session.document().hasImage());
    QCOMPARE(session.document().baseLayerId(), baseId);
    QVERIFY(session.editTargetLayerId().isNull());
    QVERIFY(session.editTarget() == LayerEditTarget::Pixels);
    QVERIFY(session.channelView() == ChannelView::Composite);
    QVERIFY(!session.baselineRequiresSave());
    QVERIFY(!session.propertyUndoActive());
    QVERIFY(session.propertyUndoText().isEmpty());
    QVERIFY(session.selectedLayerIds().isEmpty());
    QVERIFY(session.thumbnailCache().isEmpty());
    QVERIFY(session.viewState().fitToView);
    QVERIFY(!session.viewState().valid);
    QCOMPARE(session.undoStack()->count(), 0);
    QVERIFY(session.undoStack()->isClean());
    QCOMPARE(session.maskHistoryStats()->tileCount, qint64(0));
    QVERIFY(session.renderSerial() != beforeSerial);
}

void CoreTests::documentSessionRenderSerialsAreUniqueAndAdvance()
{
    DocumentSession first;
    DocumentSession second;
    const quint64 firstInitial = first.renderSerial();
    const quint64 secondInitial = second.renderSerial();
    QVERIFY(firstInitial != secondInitial);

    first.advanceRenderSerial();
    QVERIFY(first.renderSerial() != firstInitial);
    QVERIFY(first.renderSerial() != second.renderSerial());
    QCOMPARE(second.renderSerial(), secondInitial);
}

void CoreTests::undoGroupRoutesTheActiveSessionStack()
{
    DocumentSession first;
    DocumentSession second;
    QUndoGroup group;
    group.addStack(first.undoStack());
    group.addStack(second.undoStack());

    first.undoStack()->push(new QUndoCommand(QStringLiteral("First")));
    second.undoStack()->push(new QUndoCommand(QStringLiteral("Second")));
    QCOMPARE(first.undoStack()->index(), 1);
    QCOMPARE(second.undoStack()->index(), 1);

    group.setActiveStack(first.undoStack());
    group.undo();
    QCOMPARE(first.undoStack()->index(), 0);
    QCOMPARE(second.undoStack()->index(), 1);

    group.setActiveStack(second.undoStack());
    group.undo();
    QCOMPARE(first.undoStack()->index(), 0);
    QCOMPARE(second.undoStack()->index(), 0);
}



void CoreTests::sessionSnapshotRoundTripPreservesExactDocumentAndEditorState()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    SessionCacheStore store(temporaryDirectory.path());
    QVERIFY(store.isAvailable());

    DocumentSession original;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Cold Storage Test");
    settings.pixelSize = QSize(19, 13);
    settings.bitDepth = 16;
    settings.colourModel = DocumentColourModel::Rgb;
    settings.colourSpace = QColorSpace(QColorSpace::SRgbLinear);
    settings.backgroundColour = QColor(12, 34, 56, 0);
    settings.resolutionX = 144.0;
    settings.resolutionY = 96.0;
    QString error;
    QVERIFY2(original.document().createNewDocument(settings, &error), qPrintable(error));

    const QUuid rasterId = original.document().addRasterLayer(original.document().baseLayerId());
    QVERIFY(!rasterId.isNull());
    QImage raster(settings.pixelSize, QImage::Format_RGBA64);
    raster.setColorSpace(settings.colourSpace);
    for (int y = 0; y < raster.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(raster.scanLine(y));
        for (int x = 0; x < raster.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(1000 + x * 101),
                static_cast<quint16>(2000 + y * 151),
                static_cast<quint16>(3000 + (x + y) * 73),
                static_cast<quint16>((x + y) % 3 == 0 ? 0 : 65535));
        }
    }
    QVERIFY(original.document().updateLayer(rasterId, [&](LayerNode &layer) {
        layer.name = QStringLiteral("Packed 16-bit Raster");
        layer.rasterImage = raster;
        layer.rasterReferenceSize = QSize(18, 12);
        layer.rasterReferenceOrigin = QPointF(-2.0, 1.0);
        layer.transform = QTransform::fromTranslate(3.25, -1.5);
    }));
    QVERIFY(original.document().addMask(rasterId));
    QImage mask(settings.pixelSize, QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            row[x] = static_cast<uchar>((x * 17 + y * 29) & 0xff);
        }
        if (mask.bytesPerLine() > mask.width()) {
            std::memset(row + mask.width(),
                        0xa5,
                        static_cast<std::size_t>(mask.bytesPerLine() - mask.width()));
        }
    }
    QVERIFY(original.document().updateLayer(rasterId, [&](LayerNode &layer) {
        layer.maskImage = mask;
        layer.maskReferenceSize = QSize(19, 13);
        layer.maskReferenceOrigin = QPointF(-3.0, -2.0);
        layer.maskInverted = true;
    }));

    const QUuid vectorId = original.document().addVectorShape(
        VectorShapeType::RoundedRectangle, QRectF(-4.5, 2.25, 13.0, 8.5),
        QColor::fromRgba64(QRgba64::fromRgba64(12000, 33000, 51000, 47000)),
        {}, 2.75);
    QVERIFY(!vectorId.isNull());
    QVERIFY(original.document().updateLayer(vectorId, [](LayerNode &layer) {
        layer.name = QStringLiteral("Resident Rounded Shape");
        layer.opacity = 0.73;
        layer.blendMode = BlendMode::Screen;
        layer.vectorData.featherRadius = 9.25;
        layer.transform = QTransform::fromTranslate(1.25, -0.75)
            * QTransform::fromScale(1.1, 0.9);
        VectorShape &shape = layer.vectorData.objects.first();
        shape.fill.opacity = 0.61;
        shape.transform = QTransform::fromTranslate(0.5, 1.0);
        ++shape.revision;
        layer.vectorData.normalise();
    }));

    VectorBezierPath residentPath;
    VectorPathNode pathStart;
    pathStart.anchor = QPointF(1.0, 10.0);
    pathStart.outHandle = QPointF(5.0, 2.0);
    pathStart.outHandleActive = true;
    pathStart.mode = VectorNodeMode::Smooth;
    VectorPathNode pathEnd;
    pathEnd.anchor = QPointF(17.0, 9.0);
    pathEnd.inHandle = QPointF(12.0, 3.0);
    pathEnd.inHandleActive = true;
    pathEnd.mode = VectorNodeMode::Smooth;
    residentPath.nodes = {pathStart, pathEnd};
    residentPath.normalise();
    const QUuid pathId = original.document().addVectorPath(
        residentPath, QColor::fromRgba64(
            QRgba64::fromRgba64(51000, 9000, 32000, 60000)));
    QVERIFY(!pathId.isNull());
    const VectorLayerData expectedPathData = original.document()
        .layerById(pathId).vectorData;

    VectorBezierPath residentCornerPath;
    residentCornerPath.closed = true;
    for (const QPointF anchor : {QPointF(2.0, 2.0), QPointF(16.0, 2.0),
                                 QPointF(16.0, 11.0), QPointF(2.0, 11.0)}) {
        VectorPathNode node;
        node.anchor = anchor;
        node.clearHandles();
        residentCornerPath.nodes.push_back(node);
    }
    residentCornerPath.nodes[0].cornerRadius = 3.0;
    residentCornerPath.nodes[0].cornerStyle = VectorCornerStyle::Rounded;
    residentCornerPath.nodes[1].cornerRadius = 2.0;
    residentCornerPath.nodes[1].cornerStyle = VectorCornerStyle::Cutout;
    residentCornerPath.normalise();
    const QUuid cornerPathId = original.document().addVectorPath(
        residentCornerPath, QColor(20, 170, 210, 220));
    QVERIFY(!cornerPathId.isNull());
    const VectorLayerData expectedCornerPathData = original.document()
        .layerById(cornerPathId).vectorData;
    QVERIFY(expectedCornerPathData.objects.constFirst().bezierPath.hasLiveCorners());

    VectorBezierPath compoundOuter;
    compoundOuter.closed = true;
    for (const QPointF anchor : {QPointF(1.0, 1.0), QPointF(18.0, 1.0),
                                 QPointF(18.0, 12.0), QPointF(1.0, 12.0)}) {
        VectorPathNode node;
        node.anchor = anchor;
        node.clearHandles();
        compoundOuter.nodes.push_back(node);
    }
    compoundOuter.normalise();
    VectorBezierPath compoundInner;
    compoundInner.closed = true;
    for (const QPointF anchor : {QPointF(5.0, 4.0), QPointF(14.0, 4.0),
                                 QPointF(14.0, 9.0), QPointF(5.0, 9.0)}) {
        VectorPathNode node;
        node.anchor = anchor;
        node.clearHandles();
        compoundInner.nodes.push_back(node);
    }
    compoundInner.normalise();
    const QUuid compoundPathId = original.document().addVectorPath(
        compoundOuter, QColor(88, 94, 180, 220));
    QVERIFY(!compoundPathId.isNull());
    QVERIFY(original.document().updateLayer(compoundPathId,
                                             [compoundInner](LayerNode &layer) {
        VectorShape &shape = layer.vectorData.objects.first();
        shape.additionalBezierPaths = {compoundInner};
        shape.fill.enabled = true;
        shape.fill.colour = QColor(88, 94, 180, 220);
        shape.stroke.enabled = false;
        ++shape.revision;
        layer.vectorData.normalise();
    }));
    const VectorLayerData expectedCompoundPathData = original.document()
        .layerById(compoundPathId).vectorData;
    QCOMPARE(expectedCompoundPathData.objects.constFirst()
                 .additionalBezierPaths.size(), 1);

    const QUuid mixerId = original.document().addAdjustment(AdjustmentType::ChannelMixer);
    const QUuid blackWhiteId = original.document().addAdjustment(AdjustmentType::BlackAndWhite);
    const QUuid gradientId = original.document().addAdjustment(AdjustmentType::GradientMap);
    const QUuid lutId = original.document().addAdjustment(AdjustmentType::Lut);
    const QUuid gaussianId = original.document().addAdjustment(AdjustmentType::GaussianBlur);
    const QUuid unsharpId = original.document().addAdjustment(AdjustmentType::UnsharpMask);
    const QUuid invertId = original.document().addAdjustment(AdjustmentType::Invert);
    const QUuid photoFilterId = original.document().addAdjustment(AdjustmentType::PhotoFilter);
    const QUuid selectiveColourId = original.document().addAdjustment(
        AdjustmentType::SelectiveColour);
    const QUuid vignetteId = original.document().addAdjustment(AdjustmentType::Vignette);
    const QUuid rgbSplitId = original.document().addAdjustment(AdjustmentType::RgbSplit);
    const QUuid caCorrectionId = original.document().addAdjustment(
        AdjustmentType::ChromaticAberrationCorrection);
    QVERIFY(!mixerId.isNull() && !blackWhiteId.isNull() && !gradientId.isNull()
            && !lutId.isNull() && !gaussianId.isNull() && !unsharpId.isNull()
            && !invertId.isNull() && !photoFilterId.isNull()
            && !selectiveColourId.isNull() && !vignetteId.isNull()
            && !rgbSplitId.isNull() && !caCorrectionId.isNull());
    ChannelMixerParameters cacheMixer;
    cacheMixer.output(ChannelMixerOutput::Red) = {115.0, -12.0, 4.0, 1.0};
    cacheMixer.output(ChannelMixerOutput::Green) = {-8.0, 104.0, 6.0, -2.0};
    cacheMixer.output(ChannelMixerOutput::Blue) = {3.0, 14.0, 88.0, 0.0};
    BlackAndWhiteParameters cacheBlackWhite;
    cacheBlackWhite.colourWeights = {140.0, 85.0, 110.0, 75.0, 125.0, 95.0};
    cacheBlackWhite.tintEnabled = true;
    cacheBlackWhite.tintHue = 205.0;
    cacheBlackWhite.tintSaturation = 18.0;
    GradientMapParameters cacheGradient;
    cacheGradient.stops = {{0.0, QColor(6, 10, 28)},
                           {0.45, QColor(110, 42, 132)},
                           {1.0, QColor(252, 228, 182)}};
    cacheGradient.interpolation = GradientInterpolation::Smooth;
    LutParameters cacheLut;
    cacheLut.title = QStringLiteral("Embedded session LUT");
    cacheLut.sourceName = QStringLiteral("removed.cube");
    cacheLut.shaperSize = 3;
    cacheLut.shaperData = {0.0f, 0.0f, 0.0f,
                           0.42f, 0.55f, 0.61f,
                           1.0f, 1.0f, 1.0f};
    cacheLut.strength = 72.0;
    cacheLut.normalise();
    GaussianBlurParameters cacheGaussian;
    cacheGaussian.radius = 27.5;
    cacheGaussian.affectAlpha = false;
    cacheGaussian.normalise();
    UnsharpMaskParameters cacheUnsharp;
    cacheUnsharp.radius = 6.5;
    cacheUnsharp.amount = 175.0;
    cacheUnsharp.threshold = 9.0;
    cacheUnsharp.normalise();
    PhotoFilterParameters cachePhotoFilter;
    cachePhotoFilter.colour = QColor(55, 145, 218);
    cachePhotoFilter.density = 43.0;
    cachePhotoFilter.preserveLuminosity = false;
    cachePhotoFilter.normalise();
    SelectiveColourParameters cacheSelectiveColour;
    cacheSelectiveColour.range(SelectiveColourRange::Reds) = {18.0, -9.0, 12.0, 4.0};
    cacheSelectiveColour.range(SelectiveColourRange::Neutrals) = {3.0, -5.0, 7.0, 4.0};
    cacheSelectiveColour.range(SelectiveColourRange::Blacks).black = 11.0;
    cacheSelectiveColour.method = SelectiveColourMethod::Absolute;
    cacheSelectiveColour.normalise();
    VignetteParameters cacheVignette;
    cacheVignette.amount = 38.0;
    cacheVignette.midpoint = 57.0;
    cacheVignette.roundness = -24.0;
    cacheVignette.feather = 76.0;
    cacheVignette.centreX = 8.0;
    cacheVignette.centreY = -6.0;
    cacheVignette.rotation = 19.0;
    cacheVignette.highlightProtection = 33.0;
    cacheVignette.tintEnabled = true;
    cacheVignette.tint = QColor(54, 31, 17);
    cacheVignette.normalise();
    RgbSplitParameters cacheRgbSplit {-7.5, 2.25, 6.75, -3.5};
    cacheRgbSplit.normalise();
    ChromaticAberrationCorrectionParameters cacheCaCorrection {
        2.75, -2.25, 4.0, -7.0, 1.55};
    cacheCaCorrection.normalise();
    QVERIFY(original.document().updateLayer(mixerId, [cacheMixer](LayerNode &layer) {
        layer.setChannelMixerParameters(cacheMixer);
    }));
    QVERIFY(original.document().updateLayer(blackWhiteId, [cacheBlackWhite](LayerNode &layer) {
        layer.setBlackAndWhiteParameters(cacheBlackWhite);
    }));
    QVERIFY(original.document().updateLayer(gradientId, [cacheGradient](LayerNode &layer) {
        layer.setGradientMapParameters(cacheGradient);
    }));
    QVERIFY(original.document().updateLayer(lutId, [cacheLut](LayerNode &layer) {
        layer.setLutParameters(cacheLut);
    }));
    QVERIFY(original.document().updateLayer(gaussianId, [cacheGaussian](LayerNode &layer) {
        layer.setGaussianBlurParameters(cacheGaussian);
    }));
    QVERIFY(original.document().updateLayer(unsharpId, [cacheUnsharp](LayerNode &layer) {
        layer.setUnsharpMaskParameters(cacheUnsharp);
    }));
    QVERIFY(original.document().updateLayer(invertId, [](LayerNode &layer) {
        layer.setInvertParameters();
    }));
    QVERIFY(original.document().updateLayer(photoFilterId, [cachePhotoFilter](LayerNode &layer) {
        layer.setPhotoFilterParameters(cachePhotoFilter);
    }));
    QVERIFY(original.document().updateLayer(
        selectiveColourId, [cacheSelectiveColour](LayerNode &layer) {
            layer.setSelectiveColourParameters(cacheSelectiveColour);
        }));
    QVERIFY(original.document().updateLayer(vignetteId, [cacheVignette](LayerNode &layer) {
        layer.setVignetteParameters(cacheVignette);
    }));
    QVERIFY(original.document().updateLayer(rgbSplitId, [cacheRgbSplit](LayerNode &layer) {
        layer.setRgbSplitParameters(cacheRgbSplit);
    }));
    QVERIFY(original.document().updateLayer(
        caCorrectionId, [cacheCaCorrection](LayerNode &layer) {
            layer.setChromaticAberrationCorrectionParameters(cacheCaCorrection);
        }));

    SmartSourceDescriptor sessionSmartSource;
    sessionSmartSource.name = QStringLiteral("Cold Storage Smart Source");
    QVERIFY2(original.document().registerSmartSource(sessionSmartSource, &error),
             qPrintable(error));
    LayerNode sessionSmartLayer;
    sessionSmartLayer.type = LayerType::Smart;
    sessionSmartLayer.name = QStringLiteral("Resident Smart Instance");
    sessionSmartLayer.smartSource.sourceId = sessionSmartSource.id;
    sessionSmartLayer.smartSource.observedSourceRevision = sessionSmartSource.revision;
    sessionSmartLayer.opacity = 0.82;
    sessionSmartLayer.blendMode = BlendMode::Multiply;
    sessionSmartLayer.transform = QTransform::fromTranslate(-4.25, 3.5)
        * QTransform::fromScale(0.75, 1.25);
    sessionSmartLayer.smartTransform.interpolation = TransformInterpolation::Lanczos3;
    LiveFilter sessionFilter;
    sessionFilter.adjustment.reset(AdjustmentType::HighPass);
    auto sessionHighPass = std::get<HighPassParameters>(sessionFilter.adjustment.parameters);
    sessionHighPass.radius = 7.0;
    sessionHighPass.monochrome = true;
    sessionFilter.adjustment.parameters = sessionHighPass;
    sessionFilter.maskImage = QImage(1, 1, QImage::Format_Grayscale8);
    sessionFilter.maskImage.fill(173);
    sessionFilter.maskReferenceSize = original.document().sourceImage().size();
    sessionFilter.maskReferenceOrigin = QPointF();
    sessionFilter.maskEnabled = true;
    sessionFilter.maskInverted = true;
    sessionFilter.normalise();
    sessionSmartLayer.liveFilters.push_back(sessionFilter);
    LayerEffect sessionEffect;
    sessionEffect.type = LayerEffectType::OuterGlow;
    sessionEffect.enabled = true;
    sessionEffect.colour = QColor(245, 220, 130);
    sessionEffect.effectOpacity = 0.66;
    sessionEffect.effectBlendMode = BlendMode::Screen;
    sessionEffect.spread = 25.0;
    sessionEffect.size = 12.0;
    sessionEffect.normalise();
    sessionSmartLayer.layerEffects.push_back(sessionEffect);
    LayerEffect sessionBevel;
    sessionBevel.type = LayerEffectType::BevelEmboss;
    sessionBevel.enabled = true;
    sessionBevel.bevelStyle = LayerEffectBevelStyle::PillowEmboss;
    sessionBevel.bevelDirection = LayerEffectBevelDirection::Down;
    sessionBevel.bevelDepth = 225.0;
    sessionBevel.size = 9.0;
    sessionBevel.bevelSoften = 2.0;
    sessionBevel.angleDegrees = -35.0;
    sessionBevel.bevelAltitudeDegrees = 47.0;
    sessionBevel.bevelHighlightOpacity = 0.61;
    sessionBevel.bevelShadowOpacity = 0.72;
    sessionBevel.normalise();
    sessionSmartLayer.layerEffects.push_back(sessionBevel);
    QVERIFY2(original.document().insertLayerAt(
                 sessionSmartLayer, {}, original.document().layers().size()),
             "Could not add the Smart Layer session-cache fixture");

    original.document().setGuides({2.5, 8.0}, {1.0, 11.75});
    original.document().selectionMask().selectNone();
    QVERIFY(original.document().selectionMask().setCoverageRect(QRect(4, 3, 9, 6), 188));
    original.selectionEdgesVisible() = false;
    original.editTargetLayerId() = rasterId;
    original.editTarget() = LayerEditTarget::Alpha;
    original.channelView() = ChannelView::Alpha;
    original.selectedLayerIds() = {rasterId};
    original.viewState().zoom = 3.5;
    original.viewState().scrollPosition = QPoint(71, 42);
    original.viewState().fitToView = false;
    original.viewState().valid = true;
    original.cropState().initialised = true;
    original.cropState().frame = QRectF(-2.0, 1.5, 15.0, 9.0);
    original.cropState().mode = CropMode::Ratio;
    original.cropState().ratioWidth = 16.0;
    original.cropState().ratioHeight = 9.0;
    original.cropState().overlay = CropOverlay::GoldenSpiral;
    original.cropState().overlayOrientation = 3;
    original.cropState().dimOpacity = 0.47;
    original.cropState().snappingEnabled = false;
    original.cropState().deleteCroppedPixels = true;
    original.cropState().straightenSampling = true;
    original.cropState().straightenAngle = -2.75;
    original.baselineRequiresSave() = true;
    original.refreshSummary();

    QString snapshotPath;
    qint64 snapshotBytes = 0;
    QVERIFY2(store.writeSnapshot(original,
                                 &snapshotPath,
                                 &snapshotBytes,
                                 &error),
             qPrintable(error));
    QVERIFY(QFileInfo::exists(snapshotPath));
    QVERIFY(snapshotBytes > 0);
    QFile snapshotFile(snapshotPath);
    QVERIFY(snapshotFile.open(QIODevice::ReadOnly));
    QDataStream snapshotStream(&snapshotFile);
    snapshotStream.setByteOrder(QDataStream::LittleEndian);
    snapshotStream.setVersion(QDataStream::Qt_6_0);
    char snapshotMagic[8] = {};
    QCOMPARE(snapshotStream.readRawData(snapshotMagic, sizeof(snapshotMagic)),
             static_cast<int>(sizeof(snapshotMagic)));
    quint32 snapshotFormatVersion = 0;
    snapshotStream >> snapshotFormatVersion;
    QCOMPARE(snapshotFormatVersion, 28u);
    snapshotFile.close();

    // Snapshot 23 predates Layer Effect stack fields. Changing only the
    // envelope version must reject the newer binary layout rather than shifting
    // subsequent layer fields and silently losing the fx definition.
    QFile preLayerEffectBytesFile(snapshotPath);
    QVERIFY(preLayerEffectBytesFile.open(QIODevice::ReadOnly));
    QByteArray preLayerEffectEnvelope = preLayerEffectBytesFile.readAll();
    preLayerEffectBytesFile.close();
    QVERIFY(preLayerEffectEnvelope.size() > 12);
    preLayerEffectEnvelope[8] = char(23);
    preLayerEffectEnvelope[9] = char(0);
    preLayerEffectEnvelope[10] = char(0);
    preLayerEffectEnvelope[11] = char(0);
    const QString dishonestPreLayerEffectPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-layer-effect-session.bin"));
    QFile dishonestPreLayerEffect(dishonestPreLayerEffectPath);
    QVERIFY(dishonestPreLayerEffect.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPreLayerEffect.write(preLayerEffectEnvelope),
             static_cast<qint64>(preLayerEffectEnvelope.size()));
    dishonestPreLayerEffect.close();
    DocumentSession rejectedPreLayerEffect;
    QString preLayerEffectError;
    QVERIFY(!store.restoreSnapshot(dishonestPreLayerEffectPath,
                                   &rejectedPreLayerEffect,
                                   &preLayerEffectError));
    QVERIFY(!preLayerEffectError.isEmpty());

    // Snapshot 24 had Layer Effect definitions but not schema-2 renderer
    // parameters. Because its binary layout is otherwise compatible, this
    // specifically exercises the authored-parameter envelope gate.
    QByteArray preShadowGlowParametersEnvelope = preLayerEffectEnvelope;
    preShadowGlowParametersEnvelope[8] = char(24);
    const QString dishonestPreShadowGlowParametersPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-shadow-glow-parameters-session.bin"));
    QFile dishonestPreShadowGlowParameters(dishonestPreShadowGlowParametersPath);
    QVERIFY(dishonestPreShadowGlowParameters.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPreShadowGlowParameters.write(preShadowGlowParametersEnvelope),
             static_cast<qint64>(preShadowGlowParametersEnvelope.size()));
    dishonestPreShadowGlowParameters.close();
    DocumentSession rejectedPreShadowGlowParameters;
    QString preShadowGlowParametersError;
    QVERIFY(!store.restoreSnapshot(dishonestPreShadowGlowParametersPath,
                                   &rejectedPreShadowGlowParameters,
                                   &preShadowGlowParametersError));
    QVERIFY(preShadowGlowParametersError.contains(
        QStringLiteral("Layer Effect"), Qt::CaseInsensitive));

    // Snapshot 25 had the 0.14.0i shadow/glow parameter schema, but not the
    // schema-3 Stroke/Overlay fields introduced by 0.14.0j. Downgrading only
    // the envelope must reject the newer authored state rather than silently
    // accepting it as a 0.14.0i snapshot.
    QByteArray preStrokeOverlayParametersEnvelope = preLayerEffectEnvelope;
    preStrokeOverlayParametersEnvelope[8] = char(25);
    const QString dishonestPreStrokeOverlayParametersPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-stroke-overlay-parameters-session.bin"));
    QFile dishonestPreStrokeOverlayParameters(dishonestPreStrokeOverlayParametersPath);
    QVERIFY(dishonestPreStrokeOverlayParameters.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPreStrokeOverlayParameters.write(preStrokeOverlayParametersEnvelope),
             static_cast<qint64>(preStrokeOverlayParametersEnvelope.size()));
    dishonestPreStrokeOverlayParameters.close();
    DocumentSession rejectedPreStrokeOverlayParameters;
    QString preStrokeOverlayParametersError;
    QVERIFY(!store.restoreSnapshot(dishonestPreStrokeOverlayParametersPath,
                                   &rejectedPreStrokeOverlayParameters,
                                   &preStrokeOverlayParametersError));
    QVERIFY(preStrokeOverlayParametersError.contains(
        QStringLiteral("Stroke/Overlay"), Qt::CaseInsensitive));

    // Snapshot 26 predates Layer Effect schema-4 Bevel & Emboss lighting
    // parameters. Downgrading only the envelope must reject current fx state.
    QByteArray preBevelParametersEnvelope = preLayerEffectEnvelope;
    preBevelParametersEnvelope[8] = char(26);
    const QString dishonestPreBevelParametersPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-bevel-parameters-session.bin"));
    QFile dishonestPreBevelParameters(dishonestPreBevelParametersPath);
    QVERIFY(dishonestPreBevelParameters.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPreBevelParameters.write(preBevelParametersEnvelope),
             static_cast<qint64>(preBevelParametersEnvelope.size()));
    dishonestPreBevelParameters.close();
    DocumentSession rejectedPreBevelParameters;
    QString preBevelParametersError;
    QVERIFY(!store.restoreSnapshot(dishonestPreBevelParametersPath,
                                   &rejectedPreBevelParameters,
                                   &preBevelParametersError));
    QVERIFY(preBevelParametersError.contains(
        QStringLiteral("Bevel"), Qt::CaseInsensitive));

    // Snapshot 22 knew Live Filters but not their schema-2 masks. Changing
    // only the envelope version must therefore be rejected rather than
    // silently discarding or misreading the new mask payload.
    QFile preFilterMaskBytesFile(snapshotPath);
    QVERIFY(preFilterMaskBytesFile.open(QIODevice::ReadOnly));
    QByteArray preFilterMaskEnvelope = preFilterMaskBytesFile.readAll();
    preFilterMaskBytesFile.close();
    QVERIFY(preFilterMaskEnvelope.size() > 12);
    preFilterMaskEnvelope[8] = char(22);
    preFilterMaskEnvelope[9] = char(0);
    preFilterMaskEnvelope[10] = char(0);
    preFilterMaskEnvelope[11] = char(0);
    const QString dishonestPreFilterMaskPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-live-filter-mask-session.bin"));
    QFile dishonestPreFilterMask(dishonestPreFilterMaskPath);
    QVERIFY(dishonestPreFilterMask.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPreFilterMask.write(preFilterMaskEnvelope),
             static_cast<qint64>(preFilterMaskEnvelope.size()));
    dishonestPreFilterMask.close();
    DocumentSession rejectedPreFilterMask;
    QString preFilterMaskError;
    QVERIFY(!store.restoreSnapshot(dishonestPreFilterMaskPath,
                                   &rejectedPreFilterMask,
                                   &preFilterMaskError));
    QVERIFY(!preFilterMaskError.isEmpty());

    // A snapshot claiming the pre-Live-Filter envelope must not be allowed to
    // retain the new per-Smart-instance filter payload. The shifted binary
    // layout must be rejected rather than silently reinterpreted as older
    // layer fields.
    QFile preLiveFilterBytesFile(snapshotPath);
    QVERIFY(preLiveFilterBytesFile.open(QIODevice::ReadOnly));
    QByteArray preLiveFilterEnvelope = preLiveFilterBytesFile.readAll();
    preLiveFilterBytesFile.close();
    QVERIFY(preLiveFilterEnvelope.size() > 12);
    preLiveFilterEnvelope[8] = char(21);
    preLiveFilterEnvelope[9] = char(0);
    preLiveFilterEnvelope[10] = char(0);
    preLiveFilterEnvelope[11] = char(0);
    const QString dishonestPreLiveFilterPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-live-filter-session.bin"));
    QFile dishonestPreLiveFilter(dishonestPreLiveFilterPath);
    QVERIFY(dishonestPreLiveFilter.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPreLiveFilter.write(preLiveFilterEnvelope),
             static_cast<qint64>(preLiveFilterEnvelope.size()));
    dishonestPreLiveFilter.close();
    DocumentSession rejectedPreLiveFilter;
    QString preLiveFilterError;
    QVERIFY(!store.restoreSnapshot(dishonestPreLiveFilterPath,
                                   &rejectedPreLiveFilter,
                                   &preLiveFilterError));
    QVERIFY(!preLiveFilterError.isEmpty());

    QFile preSmartTransformBytesFile(snapshotPath);
    QVERIFY(preSmartTransformBytesFile.open(QIODevice::ReadOnly));
    QByteArray preSmartTransformEnvelope = preSmartTransformBytesFile.readAll();
    preSmartTransformBytesFile.close();
    QVERIFY(preSmartTransformEnvelope.size() > 12);
    preSmartTransformEnvelope[8] = char(20);
    preSmartTransformEnvelope[9] = char(0);
    preSmartTransformEnvelope[10] = char(0);
    preSmartTransformEnvelope[11] = char(0);
    const QString dishonestPreSmartTransformPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-smart-transform-session.bin"));
    QFile dishonestPreSmartTransform(dishonestPreSmartTransformPath);
    QVERIFY(dishonestPreSmartTransform.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestPreSmartTransform.write(preSmartTransformEnvelope),
             static_cast<qint64>(preSmartTransformEnvelope.size()));
    dishonestPreSmartTransform.close();
    DocumentSession rejectedPreSmartTransform;
    QString preSmartTransformError;
    QVERIFY(!store.restoreSnapshot(dishonestPreSmartTransformPath,
                                   &rejectedPreSmartTransform,
                                   &preSmartTransformError));
    QVERIFY(!preSmartTransformError.isEmpty());

    // A snapshot that claims the pre-Feather residency envelope must not be
    // allowed to smuggle in non-zero schema-8 Feather state. This protects
    // Hot/Warm/Cold migration from silently changing project semantics.
    QFile snapshotBytesFile(snapshotPath);
    QVERIFY(snapshotBytesFile.open(QIODevice::ReadOnly));
    QByteArray legacyEnvelope = snapshotBytesFile.readAll();
    snapshotBytesFile.close();
    QVERIFY(legacyEnvelope.size() > 12);
    legacyEnvelope[8] = char(16);
    legacyEnvelope[9] = char(0);
    legacyEnvelope[10] = char(0);
    legacyEnvelope[11] = char(0);
    const QString dishonestLegacyPath = temporaryDirectory.filePath(
        QStringLiteral("dishonest-pre-feather-session.bin"));
    QFile dishonestLegacy(dishonestLegacyPath);
    QVERIFY(dishonestLegacy.open(QIODevice::WriteOnly));
    QCOMPARE(dishonestLegacy.write(legacyEnvelope),
             static_cast<qint64>(legacyEnvelope.size()));
    dishonestLegacy.close();
    DocumentSession rejectedLegacy;
    QString legacyError;
    QVERIFY(!store.restoreSnapshot(dishonestLegacyPath, &rejectedLegacy,
                                   &legacyError));
    QVERIFY(legacyError.contains(QStringLiteral("Feather"), Qt::CaseInsensitive));

    DocumentSession restored;
    QVERIFY2(store.restoreSnapshot(snapshotPath, &restored, &error), qPrintable(error));
    QVERIFY(restored.document().hasImage());
    QCOMPARE(restored.document().documentName(), QStringLiteral("Cold Storage Test"));
    QVERIFY(restored.document().colourModel() == DocumentColourModel::Rgb);
    QVERIFY(restored.document().isBlankDocument());
    QVERIFY(restored.document().isModified());
    QVERIFY(std::abs(restored.document().resolutionX() - 144.0) < 0.001);
    QVERIFY(std::abs(restored.document().resolutionY() - 96.0) < 0.001);
    QVERIFY(exactImagesEqual(original.document().sourceImage(),
                             restored.document().sourceImage()));
    const LayerNode restoredRaster = restored.document().layerById(rasterId);
    QCOMPARE(restoredRaster.name, QStringLiteral("Packed 16-bit Raster"));
    QVERIFY(std::get<ChannelMixerParameters>(
        restored.document().layerById(mixerId).effectiveAdjustmentData().parameters) == cacheMixer);
    QVERIFY(std::get<BlackAndWhiteParameters>(
        restored.document().layerById(blackWhiteId).effectiveAdjustmentData().parameters) == cacheBlackWhite);
    QVERIFY(std::get<GradientMapParameters>(
        restored.document().layerById(gradientId).effectiveAdjustmentData().parameters) == cacheGradient);
    QVERIFY(std::get<LutParameters>(
        restored.document().layerById(lutId).effectiveAdjustmentData().parameters) == cacheLut);
    QVERIFY(std::get<GaussianBlurParameters>(
        restored.document().layerById(gaussianId).effectiveAdjustmentData().parameters)
            == cacheGaussian);
    QVERIFY(std::get<UnsharpMaskParameters>(
        restored.document().layerById(unsharpId).effectiveAdjustmentData().parameters)
            == cacheUnsharp);
    QVERIFY(std::holds_alternative<InvertParameters>(
        restored.document().layerById(invertId).effectiveAdjustmentData().parameters));
    QVERIFY(std::get<PhotoFilterParameters>(
        restored.document().layerById(photoFilterId).effectiveAdjustmentData().parameters)
            == cachePhotoFilter);
    QVERIFY(std::get<SelectiveColourParameters>(
        restored.document().layerById(selectiveColourId)
            .effectiveAdjustmentData().parameters) == cacheSelectiveColour);
    QVERIFY(std::get<VignetteParameters>(
        restored.document().layerById(vignetteId)
            .effectiveAdjustmentData().parameters) == cacheVignette);
    QVERIFY(std::get<RgbSplitParameters>(
        restored.document().layerById(rgbSplitId)
            .effectiveAdjustmentData().parameters) == cacheRgbSplit);
    QVERIFY(std::get<ChromaticAberrationCorrectionParameters>(
        restored.document().layerById(caCorrectionId)
            .effectiveAdjustmentData().parameters) == cacheCaCorrection);
    QVERIFY(exactImagesEqual(raster, restoredRaster.rasterImage));
    QVERIFY(exactImagesEqual(mask, restoredRaster.maskImage));
    for (int y = 0; y < restoredRaster.maskImage.height(); ++y) {
        const uchar *row = restoredRaster.maskImage.constScanLine(y);
        for (int x = restoredRaster.maskImage.width();
             x < restoredRaster.maskImage.bytesPerLine();
             ++x) {
            QCOMPARE(row[x], static_cast<uchar>(0));
        }
    }
    QVERIFY(restoredRaster.maskInverted);
    QCOMPARE(restoredRaster.rasterReferenceSize, QSize(18, 12));
    QCOMPARE(restoredRaster.rasterReferenceOrigin, QPointF(-2.0, 1.0));
    QCOMPARE(restoredRaster.maskReferenceSize, QSize(19, 13));
    QCOMPARE(restoredRaster.maskReferenceOrigin, QPointF(-3.0, -2.0));
    QVERIFY(transformsClose(restoredRaster.transform,
                            QTransform::fromTranslate(3.25, -1.5)));
    const LayerNode restoredVector = restored.document().layerById(vectorId);
    QCOMPARE(restoredVector.type, LayerType::Vector);
    QCOMPARE(restoredVector.name, QStringLiteral("Resident Rounded Shape"));
    QCOMPARE(restoredVector.vectorData, original.document().layerById(vectorId).vectorData);
    QCOMPARE(restoredVector.opacity, 0.73);
    QCOMPARE(restoredVector.blendMode, BlendMode::Screen);
    QVERIFY(transformsClose(restoredVector.transform,
                            original.document().layerById(vectorId).transform));
    const LayerNode restoredPath = restored.document().layerById(pathId);
    QCOMPARE(restoredPath.type, LayerType::Vector);
    QCOMPARE(restoredPath.vectorData, expectedPathData);
    const LayerNode restoredCornerPath = restored.document().layerById(cornerPathId);
    QCOMPARE(restoredCornerPath.type, LayerType::Vector);
    QCOMPARE(restoredCornerPath.vectorData, expectedCornerPathData);
    QVERIFY(restoredCornerPath.vectorData.objects.constFirst()
                .bezierPath.hasLiveCorners());
    const LayerNode restoredCompoundPath = restored.document().layerById(
        compoundPathId);
    QCOMPARE(restoredCompoundPath.type, LayerType::Vector);
    QCOMPARE(restoredCompoundPath.vectorData, expectedCompoundPathData);
    QCOMPARE(restoredCompoundPath.vectorData.objects.constFirst()
                 .additionalBezierPaths.size(), 1);
    QVERIFY(!restoredCompoundPath.vectorData.objects.constFirst()
                 .geometryPath().contains(QPointF(8.0, 6.0)));
    QCOMPARE(restored.document().smartSources().size(), 1);
    const SmartSourceDescriptor *restoredSmartSource = restored.document()
        .smartSources().find(sessionSmartSource.id);
    QVERIFY(restoredSmartSource != nullptr);
    QCOMPARE(restoredSmartSource->revision, sessionSmartSource.revision);
    const LayerNode restoredSmartLayer = restored.document().layerById(
        sessionSmartLayer.id);
    QCOMPARE(restoredSmartLayer.type, LayerType::Smart);
    QVERIFY(restoredSmartLayer.smartSource == sessionSmartLayer.smartSource);
    QVERIFY(restoredSmartLayer.smartTransform == sessionSmartLayer.smartTransform);
    QVERIFY(restoredSmartLayer.liveFilters == sessionSmartLayer.liveFilters);
    QVERIFY(restoredSmartLayer.layerEffects == sessionSmartLayer.layerEffects);
    QCOMPARE(restoredSmartLayer.opacity, sessionSmartLayer.opacity);
    QCOMPARE(restoredSmartLayer.blendMode, sessionSmartLayer.blendMode);
    QVERIFY(transformsClose(restoredSmartLayer.transform,
                            sessionSmartLayer.transform));
    QCOMPARE(restored.document().horizontalGuides(), QVector<double>({2.5, 8.0}));
    QCOMPARE(restored.document().verticalGuides(), QVector<double>({1.0, 11.75}));
    QVERIFY(restored.document().selectionMask().isActive());
    QCOMPARE(restored.document().selectionMask().nonZeroBounds(), QRect(4, 3, 9, 6));
    QCOMPARE(restored.document().selectionMask().coverageAt(7, 5), static_cast<quint8>(188));
    QVERIFY(!restored.selectionEdgesVisible());
    QCOMPARE(restored.editTargetLayerId(), rasterId);
    QVERIFY(restored.editTarget() == LayerEditTarget::Alpha);
    QVERIFY(restored.channelView() == ChannelView::Alpha);
    QCOMPARE(restored.selectedLayerIds(), QVector<QUuid>({rasterId}));
    QCOMPARE(restored.viewState().zoom, 3.5);
    QCOMPARE(restored.viewState().scrollPosition, QPoint(71, 42));
    QVERIFY(!restored.viewState().fitToView);
    QVERIFY(restored.viewState().valid);
    QVERIFY(restored.cropState().initialised);
    QCOMPARE(restored.cropState().frame, QRectF(-2.0, 1.5, 15.0, 9.0));
    QVERIFY(restored.cropState().mode == CropMode::Ratio);
    QCOMPARE(restored.cropState().ratioWidth, 16.0);
    QCOMPARE(restored.cropState().ratioHeight, 9.0);
    QVERIFY(restored.cropState().overlay == CropOverlay::GoldenSpiral);
    QCOMPARE(restored.cropState().overlayOrientation, 3);
    QCOMPARE(restored.cropState().dimOpacity, 0.47);
    QVERIFY(!restored.cropState().snappingEnabled);
    QVERIFY(restored.cropState().deleteCroppedPixels);
    QVERIFY(restored.cropState().straightenSampling);
    QCOMPARE(restored.cropState().straightenAngle, -2.75);
    QVERIFY(restored.baselineRequiresSave());
}

void CoreTests::damagedSessionSnapshotDoesNotReplaceResidentDocument()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    SessionCacheStore store(temporaryDirectory.path());

    DocumentSession source;
    NewDocumentSettings sourceSettings;
    sourceSettings.name = QStringLiteral("Source Snapshot");
    sourceSettings.pixelSize = QSize(32, 20);
    QString error;
    QVERIFY2(source.document().createNewDocument(sourceSettings, &error), qPrintable(error));
    QString path;
    QVERIFY2(store.writeSnapshot(source, &path, nullptr, &error), qPrintable(error));

    QFile corrupt(path);
    QVERIFY(corrupt.open(QIODevice::ReadWrite));
    QVERIFY(corrupt.size() > 64);
    QVERIFY(corrupt.resize(corrupt.size() - 7));
    corrupt.close();

    DocumentSession destination;
    NewDocumentSettings destinationSettings;
    destinationSettings.name = QStringLiteral("Must Survive");
    destinationSettings.pixelSize = QSize(7, 9);
    QVERIFY2(destination.document().createNewDocument(destinationSettings, &error),
             qPrintable(error));
    const QImage before = destination.document().sourceImage();
    QVERIFY(!store.restoreSnapshot(path, &destination, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(destination.document().documentName(), QStringLiteral("Must Survive"));
    QVERIFY(exactImagesEqual(before, destination.document().sourceImage()));
}

void CoreTests::residencyManagerEvictsOldestWarmSessionAndRestoresIt()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    DocumentResidencyManager::Limits limits;
    limits.residentDocumentBytes = 64LL * 1024LL * 1024LL;
    limits.warmSessionCount = 0;
    DocumentResidencyManager manager(limits, temporaryDirectory.path());

    DocumentSession first;
    DocumentSession second;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("First");
    settings.pixelSize = QSize(48, 32);
    settings.backgroundColour = QColor(91, 37, 211, 0);
    QString error;
    QVERIFY2(first.document().createNewDocument(settings, &error), qPrintable(error));
    const QImage firstPixels = first.document().sourceImage();
    QImage firstWorkspaceThumbnail(24, 16, QImage::Format_RGBA8888);
    firstWorkspaceThumbnail.fill(QColor(70, 20, 180, 173));
    first.workspaceThumbnail() = firstWorkspaceThumbnail;
    first.undoStack()->push(new QUndoCommand(QStringLiteral("Saved checkpoint")));
    first.undoStack()->setClean();
    first.undoStack()->push(new QUndoCommand(QStringLiteral("Dirty change")));
    first.baselineRequiresSave() = true;

    settings.name = QStringLiteral("Second");
    settings.backgroundColour = QColor(12, 220, 45, 255);
    QVERIFY2(second.document().createNewDocument(settings, &error), qPrintable(error));

    manager.registerSession(&first);
    manager.registerSession(&second);
    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Hot);
    QVERIFY2(manager.activateSession(&second, &error), qPrintable(error));

    QVERIFY(second.residency() == SessionResidency::Hot);
    QVERIFY(first.residency() == SessionResidency::Cold);
    QVERIFY(!first.document().hasImage());
    QVERIFY(!first.backingSnapshotPath().isEmpty());
    QVERIFY(QFileInfo::exists(first.backingSnapshotPath()));
    QVERIFY(first.backingSnapshotBytes() > 0);
    QVERIFY(!first.historyWasDiscardedForColdStorage());
    QCOMPARE(first.undoStack()->count(), 2);
    QCOMPARE(first.undoStack()->cleanIndex(), 1);
    QCOMPARE(first.estimatedResidentBytes(), qint64(0));
    QVERIFY(exactImagesEqual(first.workspaceThumbnail(), firstWorkspaceThumbnail));

    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Hot);
    QVERIFY(second.residency() == SessionResidency::Cold);
    QVERIFY(first.document().hasImage());
    QVERIFY(exactImagesEqual(firstPixels, first.document().sourceImage()));
    QVERIFY(exactImagesEqual(first.workspaceThumbnail(), firstWorkspaceThumbnail));
    QCOMPARE(first.document().documentName(), QStringLiteral("First"));
    QVERIFY(first.document().isModified());
    QVERIFY(first.baselineRequiresSave());
    QCOMPARE(first.undoStack()->count(), 2);
    QCOMPARE(first.undoStack()->cleanIndex(), 1);

    const DocumentResidencyManager::Stats stats = manager.stats();
    QCOMPARE(stats.registeredSessions, 2);
    QCOMPARE(stats.hotSessions, 1);
    QCOMPARE(stats.warmSessions, 0);
    QCOMPARE(stats.coldSessions, 1);
    QVERIFY(stats.backingBytes > 0);
    QCOMPARE(stats.historiesDiscardedForColdStorage, 0);
}

void CoreTests::residencyManagerPurgesColdHistoryOnlyUnderHardBudget()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    DocumentResidencyManager::Limits limits;
    limits.residentDocumentBytes = 64LL * 1024LL * 1024LL;
    limits.warmSessionCount = 0;
    DocumentResidencyManager manager(limits, temporaryDirectory.path());

    DocumentSession first;
    DocumentSession second;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(16, 16);
    QString error;
    QVERIFY2(first.document().createNewDocument(settings, &error), qPrintable(error));
    QVERIFY2(second.document().createNewDocument(settings, &error), qPrintable(error));
    first.undoStack()->push(new QUndoCommand(QStringLiteral("Heavy structural state")));
    first.structuralHistoryStats()->storedBytes = 80LL * 1024LL * 1024LL;
    first.structuralHistoryStats()->commandCount = 1;

    manager.registerSession(&first);
    manager.registerSession(&second);
    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY2(manager.activateSession(&second, &error), qPrintable(error));

    QVERIFY(first.residency() == SessionResidency::Cold);
    QVERIFY(first.historyWasDiscardedForColdStorage());
    QCOMPARE(first.undoStack()->count(), 0);
    QCOMPARE(first.estimatedHistoryBytes(), qint64(0));
    QVERIFY(QFileInfo::exists(first.backingSnapshotPath()));
    QVERIFY(first.baselineRequiresSave());

    const DocumentResidencyManager::Stats stats = manager.stats();
    QCOMPARE(stats.historiesDiscardedForColdStorage, 1);
    QCOMPARE(stats.historyBytes, qint64(0));
}

void CoreTests::tileCacheNamespacesIdenticalSurfacesByDocumentSession()
{
    TileCache cache;
    const QUuid firstSession = QUuid::createUuid();
    const QUuid secondSession = QUuid::createUuid();
    const QUuid sharedSurface = QUuid::createUuid();
    const TileAddress first {sharedSurface, 0, 0, 0, TileDomain::Raster, firstSession};
    const TileAddress second {sharedSurface, 0, 0, 0, TileDomain::Raster, secondSession};

    QImage red(8, 8, QImage::Format_RGBA8888);
    red.fill(QColor(255, 0, 0, 255));
    QImage blue(8, 8, QImage::Format_RGBA8888);
    blue.fill(QColor(0, 0, 255, 255));

    cache.beginUpdate(first, 17);
    QVERIFY(cache.publish(first, 17, red, false));
    QVERIFY(cache.markSynchronized(first, 17));
    cache.beginUpdate(second, 17);
    QVERIFY(cache.publish(second, 17, blue, false));
    QVERIFY(cache.markSynchronized(second, 17));

    const auto firstResult = cache.lookup(first, 17);
    const auto secondResult = cache.lookup(second, 17);
    QVERIFY(firstResult.has_value());
    QVERIFY(secondResult.has_value());
    QCOMPARE(firstResult->image.pixelColor(0, 0), QColor(255, 0, 0, 255));
    QCOMPARE(secondResult->image.pixelColor(0, 0), QColor(0, 0, 255, 255));
    QCOMPARE(cache.statsForSession(firstSession).residentTiles, 1);
    QCOMPARE(cache.statsForSession(secondSession).residentTiles, 1);

    cache.invalidateSurface(firstSession, sharedSurface);
    QVERIFY(!cache.lookup(first, 17).has_value());
    QVERIFY(cache.lookup(second, 17).has_value());
    QCOMPARE(cache.statsForSession(firstSession).residentTiles, 0);
    QCOMPARE(cache.statsForSession(secondSession).residentTiles, 1);

    cache.invalidateSession(secondSession);
    QCOMPARE(cache.stats().residentTiles, 0);
}

void CoreTests::renderBackendRejectsObsoleteSessionWork()
{
    RenderBackend &backend = RenderBackend::instance();
    backend.resetDocumentState();

    const QUuid sessionId = QUuid::createUuid();
    const RenderSessionContext first {sessionId, 41, 0};
    const RenderSessionContext replacement {sessionId, 42, 0};
    backend.activateSession(first);

    QImage source(16, 16, QImage::Format_RGBA8888);
    source.fill(QColor(35, 75, 125, 255));
    LayerNode base;
    base.type = LayerType::BaseImage;
    QVector<LayerNode> layers {base};

    TiledCanvasEngine::RenderInfo acceptedInfo;
    const QImage accepted = backend.renderRegion(first,
                                                  source,
                                                  layers,
                                                  source.rect(),
                                                  source.size(),
                                                  0,
                                                  nullptr,
                                                  &acceptedInfo);
    QVERIFY(!accepted.isNull());
    QVERIFY(!acceptedInfo.cancelled);

    // Advancing a DocumentSession serial does not by itself update the
    // renderer's accepted identity. This is the exact hand-off used by Crop:
    // the replacement request must remain stale until resetSessionState()
    // atomically publishes the new serial and invalidates old tile work.
    TiledCanvasEngine::RenderInfo unpublishedInfo;
    const QImage unpublished = backend.renderRegion(replacement,
                                                     source,
                                                     layers,
                                                     source.rect(),
                                                     source.size(),
                                                     0,
                                                     nullptr,
                                                     &unpublishedInfo);
    QVERIFY(unpublished.isNull());
    QVERIFY(unpublishedInfo.cancelled);

    backend.resetSessionState(replacement);
    TiledCanvasEngine::RenderInfo obsoleteInfo;
    const QImage obsolete = backend.renderRegion(first,
                                                  source,
                                                  layers,
                                                  source.rect(),
                                                  source.size(),
                                                  0,
                                                  nullptr,
                                                  &obsoleteInfo);
    QVERIFY(obsolete.isNull());
    QVERIFY(obsoleteInfo.cancelled);
    QVERIFY(obsoleteInfo.path.contains(QStringLiteral("document-session")));

    const QImage current = backend.renderRegion(replacement,
                                                 source,
                                                 layers,
                                                 source.rect(),
                                                 source.size());
    QVERIFY(!current.isNull());
    backend.resetDocumentState();
}

void CoreTests::renderBackendKeepsDisplayedInfoPerActiveSession()
{
    RenderBackend &backend = RenderBackend::instance();
    backend.resetDocumentState();

    const RenderSessionContext first {QUuid::createUuid(), 101, 0};
    const RenderSessionContext second {QUuid::createUuid(), 202, 0};
    TiledCanvasEngine::RenderInfo firstInfo;
    firstInfo.path = QStringLiteral("First session path");
    firstInfo.usedCpu = true;
    TiledCanvasEngine::RenderInfo secondInfo;
    secondInfo.path = QStringLiteral("Second session path");
    secondInfo.usedCpu = true;

    backend.activateSession(first);
    backend.setDisplayedRenderInfo(first, firstInfo, 5, 0);
    QVERIFY(backend.statusText().contains(QStringLiteral("First session path")));

    backend.activateSession(second);
    QVERIFY(!backend.statusText().contains(QStringLiteral("First session path")));
    backend.setDisplayedRenderInfo(first, firstInfo, 6, 0);
    QVERIFY(!backend.statusText().contains(QStringLiteral("First session path")));
    backend.setDisplayedRenderInfo(second, secondInfo, 7, 0);
    QVERIFY(backend.statusText().contains(QStringLiteral("Second session path")));

    backend.activateSession(first);
    QVERIFY(backend.statusText().contains(QStringLiteral("First session path")));
    QVERIFY(!backend.statusText().contains(QStringLiteral("Second session path")));

    backend.resetSessionState(RenderSessionContext {first.documentSessionId, 102, 0});
    QVERIFY(!backend.statusText().contains(QStringLiteral("First session path")));
    backend.releaseSession(second.documentSessionId);
    backend.resetDocumentState();
}

void CoreTests::newDocumentCreatesWhiteRgbBackground()
{
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Texture Sheet");
    settings.pixelSize = QSize(320, 240);
    settings.bitDepth = 8;
    settings.colourModel = DocumentColourModel::Rgb;
    settings.colourSpace = QColorSpace(QColorSpace::SRgb);
    settings.backgroundColour = QColor(255, 255, 255, 255);
    settings.resolutionX = 144.0;
    settings.resolutionY = 144.0;

    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.hasImage());
    QVERIFY(document.isBlankDocument());
    QVERIFY(document.isModified());
    QCOMPARE(document.displayName(), QStringLiteral("Texture Sheet"));
    QCOMPARE(document.documentName(), QStringLiteral("Texture Sheet"));
    QVERIFY(document.colourModel() == DocumentColourModel::Rgb);
    QCOMPARE(document.sourceImage().size(), QSize(320, 240));
    QCOMPARE(document.sourceImage().format(), QImage::Format_RGBA8888);
    QCOMPARE(document.sourceImage().pixelColor(11, 13), QColor(255, 255, 255, 255));
    QCOMPARE(document.layerCount(), 1);
    const LayerNode base = document.layerById(document.baseLayerId());
    QVERIFY(base.type == LayerType::Raster);
    QCOMPARE(base.name, QStringLiteral("Background"));
    QCOMPARE(base.rasterImage, document.sourceImage());
    QVERIFY(std::abs(document.resolutionX() - 144.0) < 0.001);
    QVERIFY(std::abs(document.resolutionY() - 144.0) < 0.001);
    QCOMPARE(document.colourProfileName(), QStringLiteral("sRGB"));
}

void CoreTests::editableRasterBaseRoundTripsWithoutDuplicatePayload()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(4, 3, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    uchar *pixel = source.scanLine(1) + 2 * 4;
    pixel[0] = 173;
    pixel[1] = 29;
    pixel[2] = 241;
    pixel[3] = 0;

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("source-backed.tga"));
    const QUuid rasterId = document.baseLayerId();
    const QString path = directory.filePath(QStringLiteral("source-backed.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(json.isObject());
    const QJsonObject root = json.object();
    QCOMPARE(QUuid(root.value(QStringLiteral("sourceRasterLayerId")).toString()),
             rasterId);
    const QJsonObject savedRaster = root.value(QStringLiteral("layerTree"))
                                        .toArray().first().toObject();
    QVERIFY(!savedRaster.contains(QStringLiteral("rasterData")));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    const LayerNode raster = loaded.layerById(rasterId);
    QVERIFY(raster.type == LayerType::Raster);
    QCOMPARE(raster.rasterImage.convertToFormat(QImage::Format_RGBA8888), source);
    QVERIFY(!loaded.isModified());
}

void CoreTests::appliedCropHistoryUsesSettledToolState()
{
    CropSessionState pending;
    pending.initialised = true;
    pending.frame = QRectF(13.0, 17.0, 301.0, 199.0);
    pending.mode = CropMode::FixedSize;
    pending.ratioWidth = 3.0;
    pending.ratioHeight = 2.0;
    pending.originalRatio = false;
    pending.fixedSize = QSize(301, 199);
    pending.overlay = CropOverlay::GoldenSpiral;
    pending.overlayOrientation = 3;
    pending.dimOpacity = 0.42;
    pending.snappingEnabled = false;
    pending.deleteCroppedPixels = true;
    pending.straightenSampling = true;
    pending.straightenAngle = 27.5;

    const QSize canvasSize(640, 480);
    const CropSessionState settled = settledCropStateForCanvas(
        pending, canvasSize);

    QVERIFY(settled.initialised);
    QCOMPARE(settled.frame, QRectF(QPointF(), QSizeF(canvasSize)));
    QCOMPARE(settled.fixedSize, canvasSize);
    QCOMPARE(settled.ratioWidth, 640.0);
    QCOMPARE(settled.ratioHeight, 480.0);
    QVERIFY(settled.originalRatio);
    QVERIFY(!settled.straightenSampling);
    QCOMPARE(settled.straightenAngle, 0.0);

    // Persistent Crop preferences remain preferences rather than becoming
    // part of the submitted straighten gesture.
    QVERIFY(settled.mode == pending.mode);
    QVERIFY(settled.overlay == pending.overlay);
    QCOMPARE(settled.overlayOrientation, pending.overlayOrientation);
    QCOMPARE(settled.dimOpacity, pending.dimOpacity);
    QCOMPARE(settled.snappingEnabled, pending.snappingEnabled);
    QCOMPARE(settled.deleteCroppedPixels, pending.deleteCroppedPixels);
}

void CoreTests::nonDestructiveCropPreservesOffCanvasRasterStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(5, 4, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(20 + x * 31);
            row[x * 4 + 1] = static_cast<uchar>(40 + y * 37);
            row[x * 4 + 2] = static_cast<uchar>(60 + (x + y) * 19);
            row[x * 4 + 3] = static_cast<uchar>((x == 2 && y == 1) ? 0 : 255);
        }
    }

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("crop-source.tga"));
    document.setGuides({1.0, 3.5}, {1.0, 4.0});
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(1, 1, 3, 2), 211));

    CropRequest request;
    request.documentRect = QRect(1, 1, 3, 2);
    request.deleteCroppedPixels = false;
    CropResult result;
    QString error;
    QVERIFY2(buildCropResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.canvasImage.size(), QSize(3, 2));
    QCOMPARE(result.layers.size(), 1);
    const LayerNode croppedLayer = result.layers.constFirst();
    QVERIFY(exactImagesEqual(croppedLayer.rasterImage, source));
    QCOMPARE(croppedLayer.rasterReferenceSize, source.size());
    QCOMPARE(croppedLayer.rasterReferenceOrigin, QPointF());
    QVERIFY(transformsClose(croppedLayer.transform,
                            QTransform::fromTranslate(-1.0, -1.0)));
    QCOMPARE(result.horizontalGuides, QVector<double>({0.0}));
    QCOMPARE(result.verticalGuides, QVector<double>({0.0, 3.0}));

    SelectionMask transformedSelection;
    QVERIFY(transformedSelection.restoreSnapshot(result.selection, false));
    QCOMPARE(transformedSelection.size(), QSize(3, 2));
    QVERIFY(transformedSelection.isActive());
    QCOMPARE(transformedSelection.nonZeroBounds(), QRect(0, 0, 3, 2));
    QCOMPARE(transformedSelection.coverageAt(1, 0), static_cast<quint8>(211));

    const QImage rendered = ImageProcessor::renderPreservingHiddenRgb(
        result.canvasImage, result.layers, nullptr, result.canvasImage.size());
    const QImage expected = source.copy(request.documentRect);
    QVERIFY(exactImagesEqual(rendered.convertToFormat(QImage::Format_RGBA8888),
                             expected));

    // The selection remains document-sized while the preserved raster keeps
    // its larger off-canvas reference extent. Brush selection sampling must
    // therefore use the snapshotted document size rather than rejecting the
    // stroke because the two extents differ.
    const QTransform layerToDocument = croppedLayer.transform;
    bool invertible = false;
    const QTransform documentToLayer = layerToDocument.inverted(&invertible);
    QVERIFY(invertible);
    TiledCanvasEngine engine;
    const auto stroke = engine.stampRasterStroke(
        croppedLayer.rasterImage,
        croppedLayer.rasterReferenceSize,
        croppedLayer.rasterImage.colorSpace(),
        croppedLayer.id,
        croppedLayer.revision,
        {QLineF(QPointF(1.5, 0.5), QPointF(1.5, 0.5))},
        documentToLayer,
        1.0,
        1.0,
        1.0,
        QColor(250, 10, 20, 255),
        false,
        false,
        QUuid(),
        &result.selection,
        layerToDocument);
    QVERIFY2(!stroke.image.isNull(), qPrintable(stroke.error));
    QVERIFY(stroke.selectionApplied);
    QCOMPARE(stroke.image.size(), source.size());

    QVERIFY(document.replaceCanvasImage(result.canvasImage));
    QVERIFY(document.replaceLayerTree(result.layers));
    QVERIFY(document.selectionMask().restoreSnapshot(result.selection, true));
    document.setGuides(result.horizontalGuides, result.verticalGuides);
    const QString path = directory.filePath(QStringLiteral("cropped.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QCOMPARE(loaded.sourceImage().size(), QSize(3, 2));
    const LayerNode loadedLayer = loaded.layerById(croppedLayer.id);
    QCOMPARE(loadedLayer.rasterReferenceSize, source.size());
    QCOMPARE(loadedLayer.rasterReferenceOrigin, QPointF());
    QVERIFY(exactImagesEqual(loadedLayer.rasterImage, source));
    QVERIFY(transformsClose(loadedLayer.transform,
                            QTransform::fromTranslate(-1.0, -1.0)));
}

void CoreTests::unclippedTransformRegionRendersOffCanvasStorage()
{
    QImage source(64, 64, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setColorSpace(QColorSpace::SRgb);

    LayerNode layer;
    layer.id = QUuid::createUuid();
    layer.name = QStringLiteral("Off-canvas rotated raster");
    layer.type = LayerType::Raster;
    layer.visible = true;
    layer.opacity = 1.0;
    layer.rasterImage = QImage(16, 16, QImage::Format_RGBA8888);
    layer.rasterImage.fill(QColor(230, 40, 30, 255));
    layer.rasterImage.setColorSpace(source.colorSpace());
    layer.rasterReferenceSize = QSize(16, 16);
    layer.rasterReferenceOrigin = QPointF(80.0, 20.0);

    const QRect requested(76, 16, 24, 24);
    const QImage rendered = ImageProcessor::renderUnclippedRegion(
        source, {layer}, requested, source.size());
    QCOMPARE(rendered.size(), requested.size());
    const QColor inside = rendered.pixelColor(8, 8);
    QVERIFY(inside.red() > 220);
    QVERIFY(inside.green() < 60);
    QVERIFY(inside.blue() < 60);
    QVERIFY(inside.alpha() > 250);
    QCOMPARE(rendered.pixelColor(1, 1).alpha(), 0);
}


void CoreTests::nonDestructiveCropExpansionKeepsNewCanvasEditable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(3, 2, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(Qt::transparent);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(31 + x * 47);
            row[x * 4 + 1] = static_cast<uchar>(53 + y * 61);
            row[x * 4 + 2] = static_cast<uchar>(79 + (x + y) * 23);
            row[x * 4 + 3] = static_cast<uchar>((x == 1 && y == 0) ? 0 : 255);
        }
    }

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("expanded-crop-source.tga"));
    CropRequest request;
    request.documentRect = QRect(-1, -1, 5, 4);
    request.deleteCroppedPixels = false;
    CropResult result;
    QString error;
    QVERIFY2(buildCropResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.canvasImage.size(), QSize(5, 4));
    QCOMPARE(result.layers.size(), 1);

    LayerNode expandedLayer = result.layers.constFirst();
    QCOMPARE(expandedLayer.rasterReferenceSize, QSize(5, 4));
    QCOMPARE(expandedLayer.rasterReferenceOrigin, QPointF(-1.0, -1.0));
    QVERIFY(transformsClose(expandedLayer.transform,
                            QTransform::fromTranslate(1.0, 1.0)));
    QCOMPARE(expandedLayer.rasterImage.size(), QSize(5, 4));
    for (int y = 0; y < source.height(); ++y) {
        const uchar *expected = source.constScanLine(y);
        const uchar *actual = expandedLayer.rasterImage.constScanLine(y + 1) + 4;
        QCOMPARE(std::memcmp(actual, expected,
                             static_cast<std::size_t>(source.width() * 4)), 0);
    }

    QImage rendered = ImageProcessor::renderPreservingHiddenRgb(
        result.canvasImage, result.layers, nullptr, result.canvasImage.size());
    QCOMPARE(rendered.pixelColor(0, 0), QColor(0, 0, 0, 0));
    QCOMPARE(rendered.pixelColor(1, 1), source.pixelColor(0, 0));
    const uchar *hiddenSource = source.constScanLine(0) + 4;
    const uchar *hiddenRendered = rendered.constScanLine(1) + 2 * 4;
    QCOMPARE(hiddenRendered[3], static_cast<uchar>(0));
    QCOMPARE(hiddenRendered[0], hiddenSource[0]);
    QCOMPARE(hiddenRendered[1], hiddenSource[1]);
    QCOMPARE(hiddenRendered[2], hiddenSource[2]);

    // Pixel (0, 0) lies in the canvas area newly exposed to the left and top.
    // The expanded storage origin must make it a real editable raster pixel,
    // not merely transparent canvas outside the layer's addressable extent.
    expandedLayer.rasterImage.setPixelColor(0, 0, QColor(231, 41, 17, 255));
    QVector<LayerNode> editedLayers = result.layers;
    editedLayers[0] = expandedLayer;
    rendered = ImageProcessor::renderPreservingHiddenRgb(
        result.canvasImage, editedLayers, nullptr, result.canvasImage.size());
    QCOMPARE(rendered.pixelColor(0, 0), QColor(231, 41, 17, 255));

    QVERIFY(document.replaceCanvasImage(result.canvasImage));
    QVERIFY(document.replaceLayerTree(editedLayers));
    const QString path = directory.filePath(QStringLiteral("expanded-crop.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    const LayerNode loadedLayer = loaded.layerById(expandedLayer.id);
    QCOMPARE(loadedLayer.rasterReferenceSize, QSize(5, 4));
    QCOMPARE(loadedLayer.rasterReferenceOrigin, QPointF(-1.0, -1.0));
    QVERIFY(transformsClose(loadedLayer.transform,
                            QTransform::fromTranslate(1.0, 1.0)));
    QVERIFY(exactImagesEqual(loadedLayer.rasterImage,
                             expandedLayer.rasterImage));
}

void CoreTests::destructiveCropCopiesStraightHiddenRgbExactly()
{
    QImage source(4, 3, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(Qt::transparent);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(11 + x * 41);
            row[x * 4 + 1] = static_cast<uchar>(17 + y * 53);
            row[x * 4 + 2] = static_cast<uchar>(23 + (x + y) * 29);
            row[x * 4 + 3] = static_cast<uchar>((x + y) % 2 == 0 ? 0 : 173);
        }
    }

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("hidden-rgb.tga"));
    CropRequest request;
    request.documentRect = QRect(1, 1, 2, 2);
    request.deleteCroppedPixels = true;
    CropResult result;
    QString error;
    QVERIFY2(buildCropResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.layers.size(), 1);
    const LayerNode croppedLayer = result.layers.constFirst();
    const QImage expected = source.copy(request.documentRect);
    QCOMPARE(croppedLayer.rasterReferenceSize, QSize(2, 2));
    QCOMPARE(croppedLayer.rasterReferenceOrigin, QPointF());
    QCOMPARE(croppedLayer.maskReferenceOrigin, QPointF());
    QVERIFY(croppedLayer.transform.isIdentity());
    QVERIFY(exactImagesEqual(croppedLayer.rasterImage, expected));
    const uchar *hidden = croppedLayer.rasterImage.constScanLine(0);
    QCOMPARE(hidden[3], static_cast<uchar>(0));
    QCOMPARE(hidden[0], expected.constScanLine(0)[0]);
    QCOMPARE(hidden[1], expected.constScanLine(0)[1]);
    QCOMPARE(hidden[2], expected.constScanLine(0)[2]);
}

void CoreTests::destructiveCropResetsEmptyRasterReferenceExtent()
{
    QImage source(6, 5, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("empty-raster-crop.tga"));
    const QUuid emptyId = document.addRasterLayer();
    QVERIFY(!emptyId.isNull());
    QVERIFY(document.updateLayer(emptyId, [](LayerNode &layer) {
        layer.rasterImage = {};
        layer.rasterReferenceSize = QSize(10, 9);
        layer.rasterReferenceOrigin = QPointF(-2.0, -3.0);
        layer.transform = QTransform::fromTranslate(4.0, 2.0);
    }));

    CropRequest request;
    request.documentRect = QRect(1, 1, 3, 2);
    request.deleteCroppedPixels = true;
    CropResult result;
    QString error;
    QVERIFY2(buildCropResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    const auto iterator = std::find_if(result.layers.cbegin(), result.layers.cend(),
                                       [emptyId](const LayerNode &layer) {
                                           return layer.id == emptyId;
                                       });
    QVERIFY(iterator != result.layers.cend());
    QVERIFY(iterator->rasterImage.isNull());
    QCOMPARE(iterator->rasterReferenceSize, QSize(3, 2));
    QCOMPARE(iterator->rasterReferenceOrigin, QPointF());
    QVERIFY(iterator->transform.isIdentity());
}

void CoreTests::canvasSizeAnchorsDistributeOddDifferencesDeterministically()
{
    const QSize oldSize(10, 8);
    const QSize expandedSize(15, 13);
    struct ExpectedRect {
        CanvasAnchor anchor;
        QPoint origin;
    };
    const ExpectedRect expanded[] = {
        {CanvasAnchor::TopLeft, QPoint(0, 0)},
        {CanvasAnchor::Top, QPoint(-2, 0)},
        {CanvasAnchor::TopRight, QPoint(-5, 0)},
        {CanvasAnchor::Left, QPoint(0, -2)},
        {CanvasAnchor::Centre, QPoint(-2, -2)},
        {CanvasAnchor::Right, QPoint(-5, -2)},
        {CanvasAnchor::BottomLeft, QPoint(0, -5)},
        {CanvasAnchor::Bottom, QPoint(-2, -5)},
        {CanvasAnchor::BottomRight, QPoint(-5, -5)},
    };
    for (const ExpectedRect &entry : expanded) {
        QCOMPARE(canvasSizeDocumentRect(oldSize, expandedSize, entry.anchor),
                 QRect(entry.origin, expandedSize));
    }

    // A centred odd contraction removes two pixels from the left/top and
    // three from the right/bottom, matching the expansion rule in reverse.
    QCOMPARE(canvasSizeDocumentRect(expandedSize,
                                    oldSize,
                                    CanvasAnchor::Centre),
             QRect(QPoint(2, 2), oldSize));
    QCOMPARE(canvasSizeDocumentRect(expandedSize,
                                    oldSize,
                                    CanvasAnchor::BottomRight),
             QRect(QPoint(5, 5), oldSize));
}

void CoreTests::canvasSizePureBoundsChangePreservesStoredPixelsAndMasks()
{
    QImage source(5, 4, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.setDotsPerMeterX(2835);
    source.setDotsPerMeterY(3779);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(17 + x * 29);
            row[x * 4 + 1] = static_cast<uchar>(31 + y * 37);
            row[x * 4 + 2] = static_cast<uchar>(47 + (x + y) * 13);
            row[x * 4 + 3] = static_cast<uchar>((x == 2 && y == 1) ? 0 : 255);
        }
    }

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-size-source.tga"));
    const QUuid baseId = document.baseLayerId();
    QVERIFY(!baseId.isNull());

    QImage mask(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            row[x] = static_cast<uchar>(45 + x * 21 + y * 9);
        }
    }
    QVERIFY(document.updateLayer(baseId, [&mask, &source](LayerNode &layer) {
        layer.maskImage = mask;
        layer.maskReferenceSize = source.size();
        layer.maskReferenceOrigin = QPointF(-1.0, 0.0);
        layer.maskEnabled = true;
    }));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(1, 1, 3, 2), 211));
    document.setGuides({0.5, 3.5}, {1.0, 4.0});

    CanvasSizeRequest request;
    request.pixelSize = QSize(8, 7);
    request.anchor = CanvasAnchor::Centre;
    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    QCOMPARE(result.canvasImage.size(), request.pixelSize);
    QCOMPARE(result.canvasImage.format(), source.format());
    QCOMPARE(result.canvasImage.colorSpace(), source.colorSpace());
    QCOMPARE(result.canvasImage.dotsPerMeterX(), source.dotsPerMeterX());
    QCOMPARE(result.canvasImage.dotsPerMeterY(), source.dotsPerMeterY());
    QCOMPARE(result.previousCanvasRect, QRect(1, 1, 5, 4));
    QCOMPARE(result.layers.size(), 1);

    const LayerNode resizedLayer = result.layers.constFirst();
    QVERIFY(exactImagesEqual(resizedLayer.rasterImage, source));
    QVERIFY(exactImagesEqual(resizedLayer.maskImage, mask));
    QCOMPARE(resizedLayer.rasterReferenceSize, source.size());
    QCOMPARE(resizedLayer.rasterReferenceOrigin, QPointF());
    QCOMPARE(resizedLayer.maskReferenceSize, source.size());
    QCOMPARE(resizedLayer.maskReferenceOrigin, QPointF(-1.0, 0.0));
    QVERIFY(transformsClose(resizedLayer.transform,
                            QTransform::fromTranslate(1.0, 1.0)));

    SelectionMask resizedSelection;
    QVERIFY(resizedSelection.restoreSnapshot(result.selection, false));
    QVERIFY(resizedSelection.isActive());
    QCOMPARE(resizedSelection.nonZeroBounds(), QRect(2, 2, 3, 2));
    QCOMPARE(resizedSelection.coverageAt(2, 2), static_cast<quint8>(211));
    QCOMPARE(result.horizontalGuides, QVector<double>({1.5, 4.5}));
    QCOMPARE(result.verticalGuides, QVector<double>({2.0, 5.0}));

    const QImage oldRendered = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, source.size());
    const QImage resizedRendered = ImageProcessor::renderPreservingHiddenRgb(
        result.canvasImage, result.layers, nullptr, request.pixelSize);
    QVERIFY(!oldRendered.isNull());
    QVERIFY(!resizedRendered.isNull());
    for (int y = 0; y < oldRendered.height(); ++y) {
        const uchar *expected = oldRendered.constScanLine(y);
        const uchar *actual = resizedRendered.constScanLine(y + 1) + 4;
        QCOMPARE(std::memcmp(actual,
                             expected,
                             static_cast<std::size_t>(oldRendered.width() * 4)),
                 0);
    }
}

void CoreTests::canvasSizeTranslatesNestedTreesAtTheRoot()
{
    QImage source(4, 4, QImage::Format_RGBA8888);
    source.fill(QColor(80, 120, 160, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-nested.tga"));
    const QUuid baseId = document.baseLayerId();
    const QUuid groupId = document.groupLayers(
        {baseId}, QStringLiteral("Nested Canvas Content"));
    QVERIFY(!groupId.isNull());

    CanvasSizeRequest request;
    request.pixelSize = QSize(7, 7);
    request.anchor = CanvasAnchor::Centre;
    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    QCOMPARE(result.layers.size(), 1);
    const LayerNode &group = result.layers.constFirst();
    QCOMPARE(group.id, groupId);
    QVERIFY(transformsClose(group.transform,
                            QTransform::fromTranslate(1.0, 1.0)));
    QCOMPARE(group.children.size(), 1);
    const LayerNode &child = group.children.constFirst();
    QCOMPARE(child.id, baseId);
    QVERIFY(child.transform.isIdentity());
    QVERIFY(exactImagesEqual(child.rasterImage, source));
}

void CoreTests::canvasSizeShrinkClipsSelectionAndGuidesWithoutDeletingPixels()
{
    QImage source(6, 6, QImage::Format_RGBA8888);
    source.fill(QColor(20, 40, 60, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-shrink.tga"));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(4, 4, 2, 2), 255));
    document.setGuides({1.0, 2.0, 5.0}, {1.0, 2.0, 5.0});

    CanvasSizeRequest request;
    request.pixelSize = QSize(2, 2);
    request.anchor = CanvasAnchor::TopLeft;
    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    SelectionMask resizedSelection;
    QVERIFY(resizedSelection.restoreSnapshot(result.selection, false));
    QVERIFY(!resizedSelection.isActive());
    QVERIFY(resizedSelection.isEmpty() == false);
    QCOMPARE(result.horizontalGuides, QVector<double>({1.0, 2.0}));
    QCOMPARE(result.verticalGuides, QVector<double>({1.0, 2.0}));
    QCOMPARE(result.layers.size(), 1);
    QVERIFY(exactImagesEqual(result.layers.constFirst().rasterImage, source));
    QCOMPARE(result.layers.constFirst().rasterReferenceSize, source.size());
    QVERIFY(result.layers.constFirst().transform.isIdentity());
}

void CoreTests::canvasSizeRoundTripPreservesOffCanvasStorage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(4, 3, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(11 + x * 41);
            row[x * 4 + 1] = static_cast<uchar>(19 + y * 53);
            row[x * 4 + 2] = static_cast<uchar>(23 + (x + y) * 17);
            row[x * 4 + 3] = static_cast<uchar>((x == 2 && y == 1) ? 0 : 207);
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("roundtrip-canvas.tga"));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(2, 1, 2, 2), 177));
    document.setGuides({1.0, 2.0}, {2.0, 3.0});

    CanvasSizeRequest request;
    request.pixelSize = QSize(2, 2);
    request.anchor = CanvasAnchor::BottomRight;
    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(document.replaceCanvasImage(result.canvasImage));
    QVERIFY(document.replaceLayerTree(result.layers));
    QVERIFY(document.selectionMask().restoreSnapshot(result.selection, true));
    document.setGuides(result.horizontalGuides, result.verticalGuides);

    const QString path = directory.filePath(QStringLiteral("canvas-size.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QCOMPARE(loaded.sourceImage().size(), QSize(2, 2));
    QCOMPARE(loaded.layers().size(), 1);
    const LayerNode loadedLayer = loaded.layers().constFirst();
    QVERIFY(exactImagesEqual(loadedLayer.rasterImage, source));
    QCOMPARE(loadedLayer.rasterReferenceSize, source.size());
    QCOMPARE(loadedLayer.rasterReferenceOrigin, QPointF());
    QVERIFY(transformsClose(loadedLayer.transform,
                            QTransform::fromTranslate(-2.0, -1.0)));
    QVERIFY(loaded.selectionMask().isActive());
    QCOMPARE(loaded.selectionMask().nonZeroBounds(), QRect(0, 0, 2, 2));
    QCOMPARE(loaded.horizontalGuides(), QVector<double>({0.0, 1.0}));
    QCOMPARE(loaded.verticalGuides(), QVector<double>({0.0, 1.0}));
}

void CoreTests::canvasSizePreservesSixteenBitHiddenRgbExactly()
{
    QImage source(3, 2, QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(1200 + x * 9000),
                static_cast<quint16>(2300 + y * 11000),
                static_cast<quint16>(3400 + (x + y) * 7000),
                static_cast<quint16>((x == 1 && y == 0) ? 0 : 42000));
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-size-16.tga"));

    CanvasSizeRequest request;
    request.pixelSize = QSize(5, 4);
    request.anchor = CanvasAnchor::BottomRight;
    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.canvasImage.format(), QImage::Format_RGBA64);
    QCOMPARE(result.layers.size(), 1);
    QVERIFY(exactImagesEqual(result.layers.constFirst().rasterImage, source));
    const auto *hidden = reinterpret_cast<const QRgba64 *>(
        result.layers.constFirst().rasterImage.constScanLine(0));
    QCOMPARE(hidden[1].alpha(), static_cast<quint16>(0));
    QCOMPARE(hidden[1].red(), static_cast<quint16>(10200));
    QCOMPARE(hidden[1].green(), static_cast<quint16>(2300));
    QCOMPARE(hidden[1].blue(), static_cast<quint16>(10400));
}

void CoreTests::canvasSizeColourFillCreatesBottomExtensionLayer()
{
    QImage source(4, 4, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(90, 110, 130, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-extension.tga"));

    CanvasSizeRequest request;
    request.pixelSize = QSize(6, 2);
    request.anchor = CanvasAnchor::TopLeft;
    request.fillMode = CanvasFillMode::Custom;
    request.fillColour = QColor(12, 34, 56, 128);

    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(result.extensionLayerCreated);
    QVERIFY(!result.destructiveClippingApplied);
    QCOMPARE(result.previousCanvasRect, QRect(0, 0, 4, 4));
    QCOMPARE(result.layers.size(), 2);

    const LayerNode &original = result.layers.constFirst();
    QVERIFY(exactImagesEqual(original.rasterImage, source));
    QCOMPARE(original.rasterReferenceSize, source.size());
    QVERIFY(original.transform.isIdentity());

    const LayerNode &extension = result.layers.constLast();
    QCOMPARE(extension.name, QStringLiteral("Canvas Extension"));
    QCOMPARE(extension.type, LayerType::Raster);
    QCOMPARE(extension.rasterReferenceSize, QSize(6, 2));
    QCOMPARE(extension.rasterReferenceOrigin, QPointF());
    QVERIFY(extension.transform.isIdentity());
    QCOMPARE(extension.rasterImage.format(), QImage::Format_RGBA8888);
    QCOMPARE(extension.rasterImage.size(), QSize(6, 2));

    // The old canvas occupies x=0..3. Only the newly exposed right strip is
    // stored on the extension layer, even though the bottom half was clipped.
    const uchar *oldArea = extension.rasterImage.constScanLine(0);
    QCOMPARE(oldArea[0], static_cast<uchar>(0));
    QCOMPARE(oldArea[1], static_cast<uchar>(0));
    QCOMPARE(oldArea[2], static_cast<uchar>(0));
    QCOMPARE(oldArea[3], static_cast<uchar>(0));
    const uchar *newArea = extension.rasterImage.constScanLine(1) + 4 * 4;
    QCOMPARE(newArea[0], static_cast<uchar>(12));
    QCOMPARE(newArea[1], static_cast<uchar>(34));
    QCOMPARE(newArea[2], static_cast<uchar>(56));
    QCOMPARE(newArea[3], static_cast<uchar>(128));
}

void CoreTests::canvasSizeColourFillPreservesSixteenBitHiddenRgb()
{
    QImage source(2, 2, QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-extension-16.tga"));

    CanvasSizeRequest request;
    request.pixelSize = QSize(4, 4);
    request.anchor = CanvasAnchor::BottomRight;
    request.fillMode = CanvasFillMode::Custom;
    request.fillColour = QColor(201, 77, 33, 0);

    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(result.extensionLayerCreated);
    QCOMPARE(result.layers.size(), 2);
    const LayerNode &extension = result.layers.constLast();
    QCOMPARE(extension.rasterImage.format(), QImage::Format_RGBA64);
    const QRgba64 expected = request.fillColour.rgba64();
    const auto *exposed = reinterpret_cast<const QRgba64 *>(
        extension.rasterImage.constScanLine(0));
    QCOMPARE(exposed[0].red(), expected.red());
    QCOMPARE(exposed[0].green(), expected.green());
    QCOMPARE(exposed[0].blue(), expected.blue());
    QCOMPARE(exposed[0].alpha(), static_cast<quint16>(0));

    // Bottom-right anchoring places the old 2x2 canvas at (2, 2); that region
    // remains pristine transparent black on the extension layer.
    const auto *oldArea = reinterpret_cast<const QRgba64 *>(
        extension.rasterImage.constScanLine(2));
    QCOMPARE(oldArea[2].red(), static_cast<quint16>(0));
    QCOMPARE(oldArea[2].green(), static_cast<quint16>(0));
    QCOMPARE(oldArea[2].blue(), static_cast<quint16>(0));
    QCOMPARE(oldArea[2].alpha(), static_cast<quint16>(0));
}

void CoreTests::canvasSizeColourFillHonoursGrayscaleDocuments()
{
    QImage source(2, 2, QImage::Format_Grayscale8);
    source.fill(91);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-extension-grey.png"));
    QCOMPARE(document.colourModel(), DocumentColourModel::Grayscale);

    CanvasSizeRequest request;
    request.pixelSize = QSize(3, 3);
    request.anchor = CanvasAnchor::TopLeft;
    request.fillMode = CanvasFillMode::Foreground;
    request.fillColour = QColor(210, 30, 90, 117);

    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(result.extensionLayerCreated);
    QCOMPARE(result.layers.size(), 2);
    const QImage &extension = result.layers.constLast().rasterImage;
    const QColor expected(qGray(request.fillColour.rgb()),
                          qGray(request.fillColour.rgb()),
                          qGray(request.fillColour.rgb()),
                          request.fillColour.alpha());
    const QColor filled = extension.pixelColor(2, 2);
    QCOMPARE(filled.red(), expected.red());
    QCOMPARE(filled.green(), expected.green());
    QCOMPARE(filled.blue(), expected.blue());
    QCOMPARE(filled.alpha(), expected.alpha());
    QCOMPARE(extension.pixelColor(0, 0), QColor(0, 0, 0, 0));
}

void CoreTests::canvasSizeColourFillSkipsPureContraction()
{
    QImage source(5, 4, QImage::Format_RGBA8888);
    source.fill(QColor(33, 66, 99, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-extension-shrink.tga"));

    CanvasSizeRequest request;
    request.pixelSize = QSize(3, 2);
    request.anchor = CanvasAnchor::Centre;
    request.fillMode = CanvasFillMode::Custom;
    request.fillColour = QColor(190, 80, 40, 255);

    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(!result.extensionLayerCreated);
    QCOMPARE(result.layers.size(), 1);
    QVERIFY(exactImagesEqual(result.layers.constFirst().rasterImage, source));
}

void CoreTests::canvasSizeExtensionLayerRoundTripsVersionSeven()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(2, 2, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(44, 66, 88, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-extension-save.tga"));

    CanvasSizeRequest request;
    request.pixelSize = QSize(4, 3);
    request.anchor = CanvasAnchor::TopLeft;
    request.fillMode = CanvasFillMode::Background;
    request.fillColour = QColor(170, 120, 70, 0);
    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(result.extensionLayerCreated);
    QVERIFY2(document.replaceStructuralState(result.canvasImage,
                                              result.layers,
                                              result.selection,
                                              result.horizontalGuides,
                                              result.verticalGuides,
                                              &error),
             qPrintable(error));

    const QString path = directory.filePath(QStringLiteral("canvas-extension.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QCOMPARE(loaded.sourceImage().size(), QSize(4, 3));
    QCOMPARE(loaded.layers().size(), 2);
    const LayerNode &extension = loaded.layers().constLast();
    QCOMPARE(extension.name, QStringLiteral("Canvas Extension"));
    QCOMPARE(extension.rasterReferenceSize, QSize(4, 3));
    QCOMPARE(extension.rasterImage.format(), QImage::Format_RGBA8888);
    const uchar *filled = extension.rasterImage.constScanLine(2) + 3 * 4;
    QCOMPARE(filled[0], static_cast<uchar>(170));
    QCOMPARE(filled[1], static_cast<uchar>(120));
    QCOMPARE(filled[2], static_cast<uchar>(70));
    QCOMPARE(filled[3], static_cast<uchar>(0));
    const uchar *oldArea = extension.rasterImage.constScanLine(0);
    QCOMPARE(oldArea[0], static_cast<uchar>(0));
    QCOMPARE(oldArea[1], static_cast<uchar>(0));
    QCOMPARE(oldArea[2], static_cast<uchar>(0));
    QCOMPARE(oldArea[3], static_cast<uchar>(0));
}

void CoreTests::canvasSizeDestructiveClipDeletesRasterAndMaskStorage()
{
    QImage source(4, 4, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(10 + x * 31);
            row[x * 4 + 1] = static_cast<uchar>(20 + y * 37);
            row[x * 4 + 2] = static_cast<uchar>(30 + (x + y) * 19);
            row[x * 4 + 3] = static_cast<uchar>((x == 3 && y == 3) ? 0 : 211);
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-destructive.tga"));
    const QUuid baseId = document.baseLayerId();
    QVERIFY(document.addMask(baseId));
    QImage mask(4, 4, QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            row[x] = static_cast<uchar>(y * 4 + x + 1);
        }
    }
    QVERIFY(document.updateLayer(baseId, [&mask](LayerNode &layer) {
        layer.maskImage = mask;
        layer.maskReferenceSize = mask.size();
        layer.maskReferenceOrigin = {};
    }));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(0, 0, 1, 1), 255));

    CanvasSizeRequest request;
    request.pixelSize = QSize(2, 2);
    request.anchor = CanvasAnchor::BottomRight;
    request.deleteOutsideCanvas = true;

    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(result.destructiveClippingApplied);
    QVERIFY(!result.extensionLayerCreated);
    QCOMPARE(result.layers.size(), 1);
    const LayerNode &clipped = result.layers.constFirst();
    QCOMPARE(clipped.id, baseId);
    QCOMPARE(clipped.rasterImage.size(), QSize(2, 2));
    QCOMPARE(clipped.rasterReferenceSize, QSize(2, 2));
    QCOMPARE(clipped.rasterReferenceOrigin, QPointF());
    QVERIFY(clipped.transform.isIdentity());

    const uchar *topLeft = clipped.rasterImage.constScanLine(0);
    const uchar *sourceTopLeft = source.constScanLine(2) + 2 * 4;
    QCOMPARE(std::memcmp(topLeft, sourceTopLeft, 4), 0);
    const uchar *hidden = clipped.rasterImage.constScanLine(1) + 4;
    const uchar *sourceHidden = source.constScanLine(3) + 3 * 4;
    QCOMPARE(std::memcmp(hidden, sourceHidden, 4), 0);
    QCOMPARE(hidden[3], static_cast<uchar>(0));
    QVERIFY(hidden[0] != 0 || hidden[1] != 0 || hidden[2] != 0);

    QCOMPARE(clipped.maskImage.size(), QSize(2, 2));
    QCOMPARE(clipped.maskReferenceSize, QSize(2, 2));
    QCOMPARE(clipped.maskReferenceOrigin, QPointF());
    QCOMPARE(clipped.maskImage.constScanLine(0)[0], static_cast<uchar>(11));
    QCOMPARE(clipped.maskImage.constScanLine(0)[1], static_cast<uchar>(12));
    QCOMPARE(clipped.maskImage.constScanLine(1)[0], static_cast<uchar>(15));
    QCOMPARE(clipped.maskImage.constScanLine(1)[1], static_cast<uchar>(16));

    SelectionMask clippedSelection;
    QVERIFY(clippedSelection.restoreSnapshot(result.selection, false));
    QVERIFY(!clippedSelection.isActive());
}

void CoreTests::canvasSizeDestructiveClipPreservesSixteenBitHiddenRgb()
{
    QImage source(3, 2, QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(1000 + x * 10000),
                static_cast<quint16>(2000 + y * 12000),
                static_cast<quint16>(3000 + (x + y) * 8000),
                static_cast<quint16>((x == 2 && y == 1) ? 0 : 51000));
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-destructive-16.tga"));

    CanvasSizeRequest request;
    request.pixelSize = QSize(2, 1);
    request.anchor = CanvasAnchor::BottomRight;
    request.deleteOutsideCanvas = true;
    CanvasSizeResult result;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(result.destructiveClippingApplied);
    QCOMPARE(result.layers.size(), 1);
    const QImage &clipped = result.layers.constFirst().rasterImage;
    QCOMPARE(clipped.format(), QImage::Format_RGBA64);
    QCOMPARE(clipped.size(), QSize(2, 1));
    const auto *actual = reinterpret_cast<const QRgba64 *>(clipped.constScanLine(0));
    const auto *expected = reinterpret_cast<const QRgba64 *>(source.constScanLine(1));
    QCOMPARE(actual[0].red(), expected[1].red());
    QCOMPARE(actual[0].green(), expected[1].green());
    QCOMPARE(actual[0].blue(), expected[1].blue());
    QCOMPARE(actual[0].alpha(), expected[1].alpha());
    QCOMPARE(actual[1].red(), expected[2].red());
    QCOMPARE(actual[1].green(), expected[2].green());
    QCOMPARE(actual[1].blue(), expected[2].blue());
    QCOMPARE(actual[1].alpha(), static_cast<quint16>(0));
    QVERIFY(actual[1].red() != 0 || actual[1].green() != 0 || actual[1].blue() != 0);
}

void CoreTests::canvasSizeDestructiveSameBoundsCompactsPreservedStorage()
{
    QImage source(4, 4, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(20 + x * 40);
            row[x * 4 + 1] = static_cast<uchar>(30 + y * 30);
            row[x * 4 + 2] = static_cast<uchar>(40 + (x + y) * 15);
            row[x * 4 + 3] = 255;
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("canvas-destructive-compact.tga"));

    CanvasSizeRequest shrink;
    shrink.pixelSize = QSize(2, 2);
    shrink.anchor = CanvasAnchor::TopLeft;
    CanvasSizeResult preserved;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, shrink, &preserved, nullptr, &error),
             qPrintable(error));
    QVERIFY(document.replaceCanvasImage(preserved.canvasImage));
    QVERIFY(document.replaceLayerTree(preserved.layers));
    QVERIFY(document.selectionMask().restoreSnapshot(preserved.selection, true));
    document.setGuides(preserved.horizontalGuides, preserved.verticalGuides);
    QCOMPARE(document.sourceImage().size(), QSize(2, 2));
    QCOMPARE(document.layers().constFirst().rasterImage.size(), QSize(4, 4));

    CanvasSizeRequest compact;
    compact.pixelSize = QSize(2, 2);
    compact.anchor = CanvasAnchor::Centre;
    compact.deleteOutsideCanvas = true;
    CanvasSizeResult clipped;
    QVERIFY2(buildCanvasSizeResult(document, compact, &clipped, nullptr, &error),
             qPrintable(error));
    QVERIFY(clipped.destructiveClippingApplied);
    QCOMPARE(clipped.layers.size(), 1);
    const LayerNode &layer = clipped.layers.constFirst();
    QCOMPARE(layer.rasterImage.size(), QSize(2, 2));
    QCOMPARE(layer.rasterReferenceSize, QSize(2, 2));
    QCOMPARE(layer.rasterReferenceOrigin, QPointF());
    QVERIFY(layer.transform.isIdentity());
    for (int y = 0; y < 2; ++y) {
        QCOMPARE(std::memcmp(layer.rasterImage.constScanLine(y),
                             source.constScanLine(y),
                             static_cast<std::size_t>(2 * 4)),
                 0);
    }
}

void CoreTests::canvasSizeCancellationLeavesNoPreparedResult()
{
    QImage source(4, 4, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("cancel-canvas-size.tga"));

    CanvasSizeRequest request;
    request.pixelSize = QSize(8, 8);
    request.anchor = CanvasAnchor::Centre;
    CanvasSizeResult result;
    std::atomic_bool cancelled {true};
    QString error;
    QVERIFY(!buildCanvasSizeResult(document,
                                   request,
                                   &result,
                                   &cancelled,
                                   &error));
    QVERIFY(result.canvasImage.isNull());
    QVERIFY(result.layers.isEmpty());
    QVERIFY(!error.isEmpty());
}

void CoreTests::canvasSizeRejectsUnpersistableSurfaceBeforeAllocation()
{
    NewDocumentSettings settings;
    settings.pixelSize = QSize(1, 1);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(10, 20, 30, 0);
    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    CanvasSizeRequest request;
    request.pixelSize = QSize(32768, 32768);
    CanvasSizeResult result;
    QVERIFY(!buildCanvasSizeResult(document, request, &result, nullptr, &error));
    QVERIFY(result.canvasImage.isNull());
    QVERIFY(result.layers.isEmpty());
    QVERIFY2(error.contains(QStringLiteral("snapshot image limit"),
                            Qt::CaseInsensitive),
             qPrintable(error));
}

void CoreTests::structuralStateReplacementIsAtomic()
{
    QImage source(6, 5, QImage::Format_RGBA8888);
    source.fill(QColor(11, 22, 33, 0));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("atomic-structural-state.tga"));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(1, 1, 3, 2), 177));
    document.setGuides({1.5, 4.0}, {2.0, 5.0});

    const QImage beforeCanvas = document.sourceImage();
    const QImage beforePreview = document.previewSource();
    const QVector<LayerNode> beforeLayers = document.layers();
    const SelectionMask::Snapshot beforeSelection = document.selectionMask().snapshot();
    const QVector<double> beforeHorizontal = document.horizontalGuides();
    const QVector<double> beforeVertical = document.verticalGuides();

    CanvasSizeRequest request;
    request.pixelSize = QSize(9, 8);
    request.anchor = CanvasAnchor::BottomRight;
    CanvasSizeResult prepared;
    QString error;
    QVERIFY2(buildCanvasSizeResult(document, request, &prepared, nullptr, &error),
             qPrintable(error));

    QVector<LayerNode> duplicateIds = prepared.layers;
    QVERIFY(!duplicateIds.isEmpty());
    duplicateIds.push_back(duplicateIds.constFirst());
    QVERIFY(!document.replaceStructuralState(prepared.canvasImage,
                                             duplicateIds,
                                             prepared.selection,
                                             prepared.horizontalGuides,
                                             prepared.verticalGuides,
                                             &error));
    QVERIFY(exactImagesEqual(document.sourceImage(), beforeCanvas));
    QVERIFY(exactImagesEqual(document.previewSource(), beforePreview));
    QVERIFY(document.layers() == beforeLayers);
    QVERIFY(document.selectionMask().snapshot().tiles == beforeSelection.tiles);
    QCOMPARE(document.horizontalGuides(), beforeHorizontal);
    QCOMPARE(document.verticalGuides(), beforeVertical);

    SelectionMask::Snapshot wrongSelection = prepared.selection;
    wrongSelection.size.rwidth() += 1;
    QVERIFY(!document.replaceStructuralState(prepared.canvasImage,
                                             prepared.layers,
                                             wrongSelection,
                                             prepared.horizontalGuides,
                                             prepared.verticalGuides,
                                             &error));
    QVERIFY(exactImagesEqual(document.sourceImage(), beforeCanvas));
    QVERIFY(exactImagesEqual(document.previewSource(), beforePreview));
    QVERIFY(document.layers() == beforeLayers);

    QVector<double> invalidGuides = prepared.horizontalGuides;
    invalidGuides.push_back(std::numeric_limits<double>::quiet_NaN());
    QVERIFY(!document.replaceStructuralState(prepared.canvasImage,
                                             prepared.layers,
                                             prepared.selection,
                                             invalidGuides,
                                             prepared.verticalGuides,
                                             &error));
    QVERIFY(exactImagesEqual(document.sourceImage(), beforeCanvas));
    QVERIFY(exactImagesEqual(document.previewSource(), beforePreview));
    QVERIFY(document.layers() == beforeLayers);

    QVERIFY2(document.replaceStructuralState(prepared.canvasImage,
                                              prepared.layers,
                                              prepared.selection,
                                              prepared.horizontalGuides,
                                              prepared.verticalGuides,
                                              &error),
             qPrintable(error));
    QCOMPARE(document.sourceImage().size(), QSize(9, 8));
    QVERIFY(exactImagesEqual(document.previewSource(), document.sourceImage()));
    QCOMPARE(document.selectionMask().size(), QSize(9, 8));
    QCOMPARE(document.selectionMask().nonZeroBounds(),
             prepared.selection.nonZeroBounds);
    QCOMPARE(document.horizontalGuides(), prepared.horizontalGuides);
    QCOMPARE(document.verticalGuides(), prepared.verticalGuides);
}

void CoreTests::imageSizeNearestScalesEditableDocumentState()
{
    QImage source(2, 2, QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const uchar values[16] = {
        10, 20, 30, 255, 40, 50, 60, 128,
        70, 80, 90, 64, 100, 110, 120, 0
    };
    for (int y = 0; y < 2; ++y) {
        std::memcpy(source.scanLine(y), values + y * 8, 8);
    }

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("image-size-nearest.tga"));
    const QUuid baseId = document.baseLayerId();
    QImage mask(2, 2, QImage::Format_Grayscale8);
    mask.scanLine(0)[0] = 0;
    mask.scanLine(0)[1] = 64;
    mask.scanLine(1)[0] = 128;
    mask.scanLine(1)[1] = 255;
    QVERIFY(document.updateLayer(baseId, [&](LayerNode &layer) {
        layer.maskImage = mask;
        layer.rasterReferenceSize = {};
        layer.rasterReferenceOrigin = QPointF(-1.0, 0.5);
        layer.maskReferenceSize = {};
        layer.maskReferenceOrigin = QPointF(0.25, -0.5);
        layer.transform = QTransform::fromTranslate(3.0, 4.0);
    }));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(1, 0, 1, 2), 200));
    document.setGuides({0.5, 1.5}, {0.25, 1.75});

    ImageSizeRequest request;
    request.pixelSize = QSize(4, 6);
    request.method = ImageResampleMethod::NearestNeighbour;
    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    QCOMPARE(result.canvasImage.size(), QSize(4, 6));
    QCOMPARE(result.layers.size(), 1);
    const LayerNode &layer = result.layers.constFirst();
    QCOMPARE(layer.rasterImage.size(), QSize(4, 6));
    QCOMPARE(layer.maskImage.size(), QSize(4, 6));
    QCOMPARE(layer.rasterReferenceSize, QSize(4, 6));
    QCOMPARE(layer.rasterReferenceOrigin, QPointF(-2.0, 1.5));
    QCOMPARE(layer.maskReferenceSize, QSize(4, 6));
    QCOMPARE(layer.maskReferenceOrigin, QPointF(0.5, -1.5));
    QVERIFY(qFuzzyCompare(layer.transform.m11(), 1.0));
    QVERIFY(qFuzzyCompare(layer.transform.m22(), 1.0));
    QVERIFY(qFuzzyCompare(layer.transform.dx(), 6.0));
    QVERIFY(qFuzzyCompare(layer.transform.dy(), 12.0));

    const uchar *topLeft = layer.rasterImage.constScanLine(0);
    QCOMPARE(topLeft[0], static_cast<uchar>(10));
    QCOMPARE(topLeft[1], static_cast<uchar>(20));
    QCOMPARE(topLeft[2], static_cast<uchar>(30));
    QCOMPARE(topLeft[3], static_cast<uchar>(255));
    const uchar *bottomRight = layer.rasterImage.constScanLine(5) + 3 * 4;
    QCOMPARE(bottomRight[0], static_cast<uchar>(100));
    QCOMPARE(bottomRight[1], static_cast<uchar>(110));
    QCOMPARE(bottomRight[2], static_cast<uchar>(120));
    QCOMPARE(bottomRight[3], static_cast<uchar>(0));

    QCOMPARE(result.selection.size, QSize(4, 6));
    SelectionMask resizedSelection(result.selection.size);
    QVERIFY(resizedSelection.restoreSnapshot(result.selection, false));
    QCOMPARE(resizedSelection.nonZeroBounds(), QRect(2, 0, 2, 6));
    QCOMPARE(resizedSelection.coverageAt(3, 5), static_cast<quint8>(200));
    QCOMPARE(result.horizontalGuides, QVector<double>({1.5, 4.5}));
    QCOMPARE(result.verticalGuides, QVector<double>({0.5, 3.5}));
}


void CoreTests::imageSizeScalesNestedCoordinateSystems()
{
    QImage source(4, 3, QImage::Format_RGBA8888);
    source.fill(QColor(30, 60, 90, 120));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("nested-image-size.tga"));

    const QUuid rasterId = document.baseLayerId();
    const QUuid groupId = document.groupLayers(
        {rasterId}, QStringLiteral("Scaled Root Group"));
    QVERIFY(!groupId.isNull());
    QVERIFY(document.updateLayer(groupId, [](LayerNode &group) {
        group.transform = QTransform::fromTranslate(5.0, 7.0);
    }));
    QVERIFY(document.updateLayer(rasterId, [](LayerNode &raster) {
        raster.transform = QTransform::fromTranslate(2.0, 3.0);
    }));

    ImageSizeRequest request;
    request.pixelSize = QSize(8, 9);
    request.method = ImageResampleMethod::NearestNeighbour;
    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    QCOMPARE(result.layers.size(), 1);
    const LayerNode &group = result.layers.constFirst();
    QCOMPARE(group.id, groupId);
    QVERIFY(qFuzzyCompare(group.transform.m11(), 1.0));
    QVERIFY(qFuzzyCompare(group.transform.m22(), 1.0));
    QVERIFY(qFuzzyCompare(group.transform.dx(), 10.0));
    QVERIFY(qFuzzyCompare(group.transform.dy(), 21.0));
    QCOMPARE(group.children.size(), 1);
    const LayerNode &raster = group.children.constFirst();
    QCOMPARE(raster.id, rasterId);
    QCOMPARE(raster.transform, QTransform::fromTranslate(4.0, 9.0));
    QCOMPARE(raster.rasterImage.size(), QSize(8, 9));
    QCOMPARE(raster.rasterReferenceSize, QSize(8, 9));
}

void CoreTests::imageSizeBilinearPreservesHiddenRgbIndependentlyOfAlpha()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    uchar *row = source.scanLine(0);
    row[0] = 200; row[1] = 10; row[2] = 20; row[3] = 0;
    row[4] = 0; row[5] = 110; row[6] = 220; row[7] = 0;

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("hidden-rgb-resize.tga"));
    ImageSizeRequest request;
    request.pixelSize = QSize(3, 1);
    request.method = ImageResampleMethod::Bilinear;
    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    const QImage resized = result.layers.constFirst().rasterImage;
    QCOMPARE(resized.size(), QSize(3, 1));
    const uchar *middle = resized.constScanLine(0) + 4;
    QCOMPARE(middle[0], static_cast<uchar>(100));
    QCOMPARE(middle[1], static_cast<uchar>(60));
    QCOMPARE(middle[2], static_cast<uchar>(120));
    QCOMPARE(middle[3], static_cast<uchar>(0));
}

void CoreTests::imageSizePreservesSixteenBitStraightComponents()
{
    QImage source(2, 2, QImage::Format_RGBA64);
    auto *row0 = reinterpret_cast<QRgba64 *>(source.scanLine(0));
    auto *row1 = reinterpret_cast<QRgba64 *>(source.scanLine(1));
    row0[0] = QRgba64::fromRgba64(1111, 2222, 3333, 0);
    row0[1] = QRgba64::fromRgba64(4444, 5555, 6666, 1);
    row1[0] = QRgba64::fromRgba64(7777, 8888, 9999, 32768);
    row1[1] = QRgba64::fromRgba64(12345, 23456, 34567, 65535);

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("image-size-16.tga"));
    ImageSizeRequest request;
    request.pixelSize = QSize(4, 4);
    request.method = ImageResampleMethod::NearestNeighbour;
    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    const QImage resized = result.layers.constFirst().rasterImage;
    QCOMPARE(resized.format(), QImage::Format_RGBA64);
    const auto *topLeft = reinterpret_cast<const QRgba64 *>(resized.constScanLine(0));
    QCOMPARE(topLeft[0].red(), static_cast<quint16>(1111));
    QCOMPARE(topLeft[0].green(), static_cast<quint16>(2222));
    QCOMPARE(topLeft[0].blue(), static_cast<quint16>(3333));
    QCOMPARE(topLeft[0].alpha(), static_cast<quint16>(0));
    const auto *bottom = reinterpret_cast<const QRgba64 *>(resized.constScanLine(3));
    QCOMPARE(bottom[3].red(), static_cast<quint16>(12345));
    QCOMPARE(bottom[3].alpha(), static_cast<quint16>(65535));
}

void CoreTests::imageSizeRoundTripsThroughVersionSevenProject()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(3, 2);
    settings.bitDepth = 16;
    settings.colourModel = DocumentColourModel::Grayscale;
    settings.backgroundColour = QColor(31, 63, 127, 0);
    settings.resolutionX = 300.0;
    settings.resolutionY = 150.0;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(1, 0, 2, 2), 137));
    document.setGuides({1.0}, {2.0});

    ImageSizeRequest request;
    request.pixelSize = QSize(6, 4);
    request.method = ImageResampleMethod::NearestNeighbour;
    ImageSizeResult prepared;
    QVERIFY2(buildImageSizeResult(document, request, &prepared, nullptr, &error),
             qPrintable(error));
    QVERIFY2(document.replaceStructuralState(prepared.canvasImage,
                                             prepared.layers,
                                             prepared.selection,
                                             prepared.horizontalGuides,
                                             prepared.verticalGuides,
                                             &error),
             qPrintable(error));

    const QString path = directory.filePath(QStringLiteral("image-size.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QCOMPARE(restored.sourceImage().size(), QSize(6, 4));
    QCOMPARE(restored.colourModel(), DocumentColourModel::Grayscale);
    QCOMPARE(restored.resolutionX(), 300.0);
    QCOMPARE(restored.resolutionY(), 150.0);
    QCOMPARE(restored.layers(), document.layers());
    QCOMPARE(restored.selectionMask().snapshot().tiles,
             document.selectionMask().snapshot().tiles);
    QCOMPARE(restored.horizontalGuides(), QVector<double>({2.0}));
    QCOMPARE(restored.verticalGuides(), QVector<double>({4.0}));
}

void CoreTests::imageSizeCancellationPublishesNoResult()
{
    QImage source(32, 32, QImage::Format_RGBA8888);
    source.fill(QColor(10, 20, 30, 40));
    PhotoDocument document;
    document.setSourceImage(source);
    ImageSizeRequest request;
    request.pixelSize = QSize(128, 128);
    ImageSizeResult result;
    std::atomic_bool cancelRequested {true};
    QString error;
    QVERIFY(!buildImageSizeResult(document,
                                  request,
                                  &result,
                                  &cancelRequested,
                                  &error));
    QVERIFY(result.canvasImage.isNull());
    QVERIFY(error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
}

void CoreTests::imageSizeNearestTieUsesExactPixelCentreMapping()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(11, 22, 33, 44));
    source.setPixelColor(1, 0, QColor(211, 222, 233, 244));

    const QImage resized = resampleStraightRgbaCpuReference(
        source, QSize(49, 1), ImageResampleMethod::NearestNeighbour);
    QVERIFY(!resized.isNull());
    // Destination x=24 maps to the exact source half-pixel tie. The defined
    // Nearest rule selects source index 1 rather than depending on f32/f64 drift.
    QCOMPARE(resized.pixelColor(24, 0), source.pixelColor(1, 0));
    QCOMPARE(resized.pixelColor(23, 0), source.pixelColor(0, 0));
}


void CoreTests::imageSizeUsesAcceleratorForEligiblePayloads()
{
    QImage source(3, 2, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4] = static_cast<uchar>(10 + x * 20);
            row[x * 4 + 1] = static_cast<uchar>(30 + y * 40);
            row[x * 4 + 2] = static_cast<uchar>(50 + x + y);
            row[x * 4 + 3] = static_cast<uchar>((x + y) == 0 ? 0 : 200);
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("accelerated-image-size.tga"));
    QImage mask(source.size(), QImage::Format_Grayscale8);
    mask.fill(127);
    QVERIFY(document.updateLayer(document.baseLayerId(), [&](LayerNode &layer) {
        layer.maskImage = mask;
    }));

    int acceleratorCalls = 0;
    ImageSizeRequest request;
    request.pixelSize = QSize(7, 5);
    request.method = ImageResampleMethod::Bilinear;
    request.accelerator = [&acceleratorCalls](
                              const QImage &payload,
                              const QSize &destinationSize,
                              const ImageResampleMethod method,
                              const std::atomic_bool *cancelRequested,
                              QString *) {
        ++acceleratorCalls;
        return payload.format() == QImage::Format_Grayscale8
            ? resampleGrayscaleCpuReference(payload,
                                            destinationSize,
                                            method,
                                            cancelRequested)
            : resampleStraightRgbaCpuReference(payload,
                                               destinationSize,
                                               method,
                                               cancelRequested);
    };

    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(acceleratorCalls, 3); // canvas, editable raster, mask
    QCOMPARE(result.gpuPayloads, 3);
    QCOMPARE(result.cpuPayloads, 0);
    QVERIFY(result.firstGpuFallbackReason.isEmpty());
    QCOMPARE(result.canvasImage.size(), request.pixelSize);
    QCOMPARE(result.layers.constFirst().maskImage.size(), request.pixelSize);
}

void CoreTests::imageSizeAcceleratorFailureFallsBackToCpuReference()
{
    QImage source(4, 3, QImage::Format_RGBA8888);
    source.fill(QColor(20, 40, 80, 0));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("fallback-image-size.tga"));

    int acceleratorCalls = 0;
    ImageSizeRequest request;
    request.pixelSize = QSize(9, 6);
    request.method = ImageResampleMethod::NearestNeighbour;
    request.accelerator = [&acceleratorCalls](const QImage &,
                                               const QSize &,
                                               ImageResampleMethod,
                                               const std::atomic_bool *,
                                               QString *errorMessage) {
        ++acceleratorCalls;
        if (errorMessage) {
            *errorMessage = QStringLiteral("Synthetic GPU fallback");
        }
        return QImage();
    };

    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(acceleratorCalls, 2); // canvas and editable raster
    QCOMPARE(result.gpuPayloads, 0);
    QCOMPARE(result.cpuPayloads, 2);
    QCOMPARE(result.firstGpuFallbackReason,
             QStringLiteral("Synthetic GPU fallback"));
    QCOMPARE(result.canvasImage.size(), request.pixelSize);
    const uchar *pixel = result.layers.constFirst().rasterImage.constScanLine(0);
    QCOMPARE(pixel[0], static_cast<uchar>(20));
    QCOMPARE(pixel[3], static_cast<uchar>(0));
}



void CoreTests::imageSizeAreaReductionUsesExactBoxAverage()
{
    QImage source(4, 1, QImage::Format_Grayscale8);
    uchar *row = source.scanLine(0);
    row[0] = 0;
    row[1] = 64;
    row[2] = 128;
    row[3] = 255;

    const QImage reduced = resampleGrayscaleCpuReference(
        source, QSize(2, 1), ImageResampleMethod::Area);
    QVERIFY(!reduced.isNull());
    QCOMPARE(reduced.format(), QImage::Format_Grayscale8);
    QCOMPARE(reduced.constScanLine(0)[0], static_cast<uchar>(32));
    QCOMPARE(reduced.constScanLine(0)[1], static_cast<uchar>(192));
}

void CoreTests::imageSizeAdvancedFiltersPreserveStraightConstantComponents()
{
    QImage source(QSize(9, 7), QImage::Format_RGBA64);
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(12345, 23456, 34567, 0);
        }
    }

    for (const ImageResampleMethod method : {
             ImageResampleMethod::Bicubic,
             ImageResampleMethod::Lanczos3,
             ImageResampleMethod::Area}) {
        const QImage resized = resampleStraightRgbaCpuReference(
            source, QSize(4, 3), method);
        QVERIFY(!resized.isNull());
        QCOMPARE(resized.format(), QImage::Format_RGBA64);
        for (int y = 0; y < resized.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(
                resized.constScanLine(y));
            for (int x = 0; x < resized.width(); ++x) {
                QCOMPARE(row[x].red(), static_cast<quint16>(12345));
                QCOMPARE(row[x].green(), static_cast<quint16>(23456));
                QCOMPARE(row[x].blue(), static_cast<quint16>(34567));
                QCOMPARE(row[x].alpha(), static_cast<quint16>(0));
            }
        }
    }
}

void CoreTests::imageSizeAdvancedMethodsStayOnCpuReference()
{
    QImage source(5, 4, QImage::Format_RGBA8888);
    source.fill(QColor(40, 80, 120, 0));
    PhotoDocument document;
    document.setSourceImage(source);

    int acceleratorCalls = 0;
    ImageSizeRequest request;
    request.pixelSize = QSize(3, 2);
    request.method = ImageResampleMethod::Lanczos3;
    request.accelerator = [&acceleratorCalls](const QImage &,
                                               const QSize &,
                                               ImageResampleMethod,
                                               const std::atomic_bool *,
                                               QString *) {
        ++acceleratorCalls;
        return QImage();
    };

    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(acceleratorCalls, 0);
    QCOMPARE(result.gpuPayloads, 0);
    QCOMPARE(result.cpuPayloads, 2); // canvas and editable Raster payload.
    QVERIFY(result.firstGpuFallbackReason.isEmpty());
}

void CoreTests::imageSizeResolutionOnlyPreservesEditablePixels()
{
    QImage source(3, 2, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4] = static_cast<uchar>(10 + x);
            row[x * 4 + 1] = static_cast<uchar>(20 + y);
            row[x * 4 + 2] = static_cast<uchar>(30 + x + y);
            row[x * 4 + 3] = static_cast<uchar>(x == 0 ? 0 : 200);
        }
    }
    PhotoDocument document;
    document.setSourceImage(source);
    const QVector<LayerNode> beforeLayers = document.layers();
    const SelectionMask::Snapshot beforeSelection = document.selectionMask().snapshot();

    ImageSizeRequest request;
    request.pixelSize = source.size();
    request.resamplePixels = false;
    request.resolutionX = 240.0;
    request.resolutionY = 180.0;
    request.method = ImageResampleMethod::Lanczos3;
    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(!result.pixelsResampled);
    QCOMPARE(result.canvasImage.size(), source.size());
    QCOMPARE(result.canvasImage.format(), source.format());
    for (int y = 0; y < source.height(); ++y) {
        QCOMPARE(std::memcmp(result.canvasImage.constScanLine(y),
                             source.constScanLine(y),
                             static_cast<std::size_t>(source.width() * 4)),
                 0);
    }
    QCOMPARE(result.layers, beforeLayers);
    QCOMPARE(result.selection.tiles, beforeSelection.tiles);
    QCOMPARE(result.resolutionX, 240.0);
    QCOMPARE(result.resolutionY, 180.0);
    QCOMPARE(result.cpuPayloads, 0);
    QCOMPARE(result.gpuPayloads, 0);

    QVERIFY2(document.replaceStructuralState(result.canvasImage,
                                             result.layers,
                                             result.selection,
                                             result.horizontalGuides,
                                             result.verticalGuides,
                                             result.resolutionX,
                                             result.resolutionY,
                                             &error),
             qPrintable(error));
    QCOMPARE(document.sourceImage().size(), source.size());
    QCOMPARE(document.sourceImage().format(), source.format());
    for (int y = 0; y < source.height(); ++y) {
        QCOMPARE(std::memcmp(document.sourceImage().constScanLine(y),
                             source.constScanLine(y),
                             static_cast<std::size_t>(source.width() * 4)),
                 0);
    }
    QCOMPARE(document.layers(), beforeLayers);
    QCOMPARE(document.resolutionX(), 240.0);
    QCOMPARE(document.resolutionY(), 180.0);
}

void CoreTests::imageSizeSamePixelSizeNeverRewritesPayloads()
{
    QImage source(QSize(3, 3), QImage::Format_RGBA64);
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = QRgba64::fromRgba64(1000 + x, 2000 + y, 3000 + x + y, 0);
        }
    }
    PhotoDocument document;
    document.setSourceImage(source);
    const QVector<LayerNode> layers = document.layers();

    ImageSizeRequest request;
    request.pixelSize = source.size();
    request.resamplePixels = true;
    request.resolutionX = 144.0;
    request.resolutionY = 144.0;
    request.method = ImageResampleMethod::Lanczos3;
    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(!result.pixelsResampled);
    QCOMPARE(result.layers, layers);
    QCOMPARE(result.cpuPayloads, 0);
    QCOMPARE(result.gpuPayloads, 0);
    for (int y = 0; y < source.height(); ++y) {
        QCOMPARE(std::memcmp(result.canvasImage.constScanLine(y),
                             document.sourceImage().constScanLine(y),
                             static_cast<std::size_t>(source.width() * 8)),
                 0);
    }
}

void CoreTests::imageSizePreflightRejectsCombinedOutputBudgetBeforeAcceleration()
{
    QImage source(QSize(4, 4), QImage::Format_RGBA8888);
    source.fill(QColor(20, 40, 60, 80));
    PhotoDocument document;
    document.setSourceImage(source);

    int acceleratorCalls = 0;
    ImageSizeRequest request;
    request.pixelSize = QSize(8, 8);
    request.method = ImageResampleMethod::Bilinear;
    request.maximumPreparedBytes = 1;
    request.accelerator = [&acceleratorCalls](const QImage &,
                                              const QSize &,
                                              ImageResampleMethod,
                                              const std::atomic_bool *,
                                              QString *) {
        ++acceleratorCalls;
        return QImage();
    };
    ImageSizeResult result;
    QString error;
    QVERIFY(!buildImageSizeResult(document, request, &result, nullptr, &error));
    QCOMPARE(acceleratorCalls, 0);
    QVERIFY2(error.contains(QStringLiteral("safe preparation budget")),
             qPrintable(error));
    QVERIFY(result.canvasImage.isNull());
}

void CoreTests::imageSizeRejectsUnknownResampleMethod()
{
    QImage source(QSize(3, 2), QImage::Format_RGBA8888);
    source.fill(QColor(12, 34, 56, 78));
    PhotoDocument document;
    document.setSourceImage(source);

    ImageSizeRequest request;
    request.pixelSize = QSize(6, 4);
    request.method = static_cast<ImageResampleMethod>(255);
    ImageSizeResult result;
    QString error;
    QVERIFY(!buildImageSizeResult(document, request, &result, nullptr, &error));
    QVERIFY2(error.contains(QStringLiteral("method"), Qt::CaseInsensitive),
             qPrintable(error));
    QVERIFY(result.canvasImage.isNull());
}

void CoreTests::imageSizePreflightRejectsNonFiniteLayerCoordinates()
{
    QImage source(QSize(4, 4), QImage::Format_RGBA8888);
    source.fill(QColor(10, 20, 30, 255));
    PhotoDocument document;
    document.setSourceImage(source);
    const QUuid baseId = document.baseLayerId();
    QVERIFY(document.updateLayer(baseId, [](LayerNode &layer) {
        QTransform invalid;
        invalid.setMatrix(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0,
                          0.0, 1.0, 0.0,
                          0.0, 0.0, 1.0);
        layer.transform = invalid;
    }));

    int acceleratorCalls = 0;
    ImageSizeRequest request;
    request.pixelSize = QSize(8, 8);
    request.method = ImageResampleMethod::NearestNeighbour;
    request.maximumPreparedBytes = 1024 * 1024;
    request.accelerator = [&acceleratorCalls](const QImage &,
                                              const QSize &,
                                              ImageResampleMethod,
                                              const std::atomic_bool *,
                                              QString *) {
        ++acceleratorCalls;
        return QImage();
    };
    ImageSizeResult result;
    QString error;
    QVERIFY(!buildImageSizeResult(document, request, &result, nullptr, &error));
    QCOMPARE(acceleratorCalls, 0);
    QVERIFY2(error.contains(QStringLiteral("invalid coordinates")),
             qPrintable(error));
}

void CoreTests::imageSizeAcceleratorMetadataIsNormalised()
{
    QImage source(QSize(2, 2), QImage::Format_RGBA8888);
    source.fill(QColor(80, 100, 120, 0));
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.setDotsPerMeterX(4724);
    source.setDotsPerMeterY(3543);
    source.setDevicePixelRatio(1.5);
    PhotoDocument document;
    document.setSourceImage(source);
    const QUuid baseId = document.baseLayerId();
    QVERIFY(document.addMask(baseId));
    QImage mask(QSize(2, 2), QImage::Format_Grayscale8);
    mask.fill(173);
    mask.setColorSpace(QColorSpace(QColorSpace::SRgb));
    mask.setDotsPerMeterX(2362);
    mask.setDotsPerMeterY(1181);
    mask.setDevicePixelRatio(1.25);
    QVERIFY(document.updateLayer(baseId, [&](LayerNode &layer) {
        layer.maskImage = mask;
        layer.maskReferenceSize = mask.size();
    }));

    ImageSizeRequest request;
    request.pixelSize = QSize(5, 3);
    request.method = ImageResampleMethod::Bilinear;
    request.resolutionX = 300.0;
    request.resolutionY = 150.0;
    request.maximumPreparedBytes = 1024 * 1024;
    request.accelerator = [](const QImage &input,
                             const QSize &destinationSize,
                             ImageResampleMethod,
                             const std::atomic_bool *,
                             QString *) {
        const bool grayscale = input.format() == QImage::Format_Grayscale8;
        QImage output(destinationSize,
                      grayscale ? QImage::Format_Grayscale8
                                : QImage::Format_RGBA8888);
        if (grayscale) {
            output.fill(173);
        } else {
            output.fill(QColor(11, 22, 33, 0));
        }
        return output;
    };

    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.gpuPayloads, 3);
    QCOMPARE(result.cpuPayloads, 0);
    QVERIFY(result.canvasImage.colorSpace() == document.sourceImage().colorSpace());
    QCOMPARE(result.canvasImage.devicePixelRatio(),
             document.sourceImage().devicePixelRatio());
    QCOMPARE(result.canvasImage.dotsPerMeterX(), qRound(300.0 / 0.0254));
    QCOMPARE(result.canvasImage.dotsPerMeterY(), qRound(150.0 / 0.0254));
    QCOMPARE(result.layers.size(), 1);
    const QImage layerPixels = result.layers.constFirst().rasterImage;
    QVERIFY(layerPixels.colorSpace() == document.sourceImage().colorSpace());
    QCOMPARE(layerPixels.devicePixelRatio(),
             document.sourceImage().devicePixelRatio());
    QCOMPARE(layerPixels.dotsPerMeterX(), document.sourceImage().dotsPerMeterX());
    QCOMPARE(layerPixels.dotsPerMeterY(), document.sourceImage().dotsPerMeterY());
    const QImage resizedMask = result.layers.constFirst().maskImage;
    QVERIFY(resizedMask.colorSpace() == mask.colorSpace());
    QCOMPARE(resizedMask.devicePixelRatio(), mask.devicePixelRatio());
    QCOMPARE(resizedMask.dotsPerMeterX(), mask.dotsPerMeterX());
    QCOMPARE(resizedMask.dotsPerMeterY(), mask.dotsPerMeterY());
    QVERIFY(result.estimatedPreparedBytes > 0);
}

void CoreTests::imageSizeCancellationAfterGpuReturnPublishesNoResult()
{
    QImage source(QSize(3, 3), QImage::Format_RGBA8888);
    source.fill(QColor(40, 80, 120, 0));
    PhotoDocument document;
    document.setSourceImage(source);

    std::atomic_bool cancelled(false);
    int acceleratorCalls = 0;
    ImageSizeRequest request;
    request.pixelSize = QSize(6, 6);
    request.method = ImageResampleMethod::NearestNeighbour;
    request.maximumPreparedBytes = 1024 * 1024;
    request.accelerator = [&cancelled, &acceleratorCalls](
                              const QImage &,
                              const QSize &destinationSize,
                              ImageResampleMethod,
                              const std::atomic_bool *,
                              QString *) {
        ++acceleratorCalls;
        QImage output(destinationSize, QImage::Format_RGBA8888);
        output.fill(QColor(1, 2, 3, 0));
        cancelled.store(true, std::memory_order_release);
        return output;
    };

    ImageSizeResult result;
    QString error;
    QVERIFY(!buildImageSizeResult(document, request, &result, &cancelled, &error));
    QCOMPARE(acceleratorCalls, 1);
    QVERIFY(result.canvasImage.isNull());
    QVERIFY2(error.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive),
             qPrintable(error));
}

void CoreTests::imageSizeHandlesEmptyLayerTree()
{
    QImage source(QSize(5, 4), QImage::Format_RGBA8888);
    source.fill(QColor(15, 25, 35, 45));
    PhotoDocument document;
    document.setSourceImage(source);
    QVERIFY(document.replaceLayerTree(QVector<LayerNode>{}));
    document.selectionMask().deactivate();
    document.setGuides({1.5}, {2.5});

    ImageSizeRequest request;
    request.pixelSize = QSize(8, 6);
    request.method = ImageResampleMethod::Area;
    request.maximumPreparedBytes = 1024 * 1024;
    ImageSizeResult result;
    QString error;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(result.layers.isEmpty());
    QVERIFY(!result.selection.active);
    QCOMPARE(result.selection.size, QSize(8, 6));
    QCOMPARE(result.horizontalGuides, QVector<double>({2.25}));
    QCOMPARE(result.verticalGuides, QVector<double>({4.0}));
    QVERIFY2(document.replaceStructuralState(result.canvasImage,
                                             result.layers,
                                             result.selection,
                                             result.horizontalGuides,
                                             result.verticalGuides,
                                             result.resolutionX,
                                             result.resolutionY,
                                             &error),
             qPrintable(error));
    QVERIFY(document.layers().isEmpty());
    QCOMPARE(document.sourceImage().size(), QSize(8, 6));
}

void CoreTests::imageSizeStateSurvivesColdResidency()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    DocumentResidencyManager::Limits limits;
    limits.residentDocumentBytes = 64LL * 1024LL * 1024LL;
    limits.warmSessionCount = 0;
    DocumentResidencyManager manager(limits, temporaryDirectory.path());

    DocumentSession first;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Image Size Cold State");
    settings.pixelSize = QSize(6, 4);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(30, 50, 70, 0);
    settings.resolutionX = 96.0;
    settings.resolutionY = 72.0;
    QString error;
    QVERIFY2(first.document().createNewDocument(settings, &error), qPrintable(error));

    const QUuid baseId = first.document().baseLayerId();
    QImage pixels(QSize(6, 4), QImage::Format_RGBA64);
    for (int y = 0; y < pixels.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(pixels.scanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(1000 + x * 101),
                static_cast<quint16>(2000 + y * 113),
                static_cast<quint16>(3000 + (x + y) * 127),
                static_cast<quint16>((x + y) % 3 == 0 ? 0 : 65535));
        }
    }
    QVERIFY(first.document().updateLayer(baseId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = QSize(8, 6);
        layer.rasterReferenceOrigin = QPointF(-1.5, 0.75);
        layer.transform.translate(0.5, -0.25);
    }));
    QVERIFY(first.document().addMask(baseId));
    QImage mask(QSize(6, 4), QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            row[x] = static_cast<uchar>(20 + x * 19 + y * 7);
        }
    }
    QVERIFY(first.document().updateLayer(baseId, [&](LayerNode &layer) {
        layer.maskImage = mask;
        layer.maskReferenceSize = QSize(7, 5);
        layer.maskReferenceOrigin = QPointF(-0.5, 1.25);
    }));
    first.document().selectionMask().selectNone();
    QVERIFY(first.document().selectionMask().setCoverageRect(QRect(1, 1, 4, 2), 137));
    first.document().setGuides({1.0, 3.5}, {0.5, 5.0});

    ImageSizeRequest request;
    request.pixelSize = QSize(9, 7);
    request.method = ImageResampleMethod::Lanczos3;
    request.resolutionX = 300.0;
    request.resolutionY = 150.0;
    request.maximumPreparedBytes = 64LL * 1024LL * 1024LL;
    ImageSizeResult prepared;
    QVERIFY2(buildImageSizeResult(first.document(), request, &prepared, nullptr, &error),
             qPrintable(error));
    QVERIFY2(first.document().replaceStructuralState(prepared.canvasImage,
                                                     prepared.layers,
                                                     prepared.selection,
                                                     prepared.horizontalGuides,
                                                     prepared.verticalGuides,
                                                     prepared.resolutionX,
                                                     prepared.resolutionY,
                                                     &error),
             qPrintable(error));
    first.refreshSummary();

    const QImage expectedCanvas = first.document().sourceImage();
    const QVector<LayerNode> expectedLayers = first.document().layers();
    const SelectionMask::Snapshot expectedSelection =
        first.document().selectionMask().snapshot();
    const QVector<double> expectedHorizontal = first.document().horizontalGuides();
    const QVector<double> expectedVertical = first.document().verticalGuides();

    DocumentSession second;
    settings.name = QStringLiteral("Image Size Eviction Driver");
    settings.pixelSize = QSize(8, 8);
    settings.bitDepth = 8;
    QVERIFY2(second.document().createNewDocument(settings, &error), qPrintable(error));
    manager.registerSession(&first);
    manager.registerSession(&second);
    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY2(manager.activateSession(&second, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Cold);
    QVERIFY(!first.document().hasImage());

    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Hot);
    QVERIFY(exactImagesEqual(first.document().sourceImage(), expectedCanvas));
    QCOMPARE(first.document().resolutionX(), 300.0);
    QCOMPARE(first.document().resolutionY(), 150.0);
    QCOMPARE(first.document().horizontalGuides(), expectedHorizontal);
    QCOMPARE(first.document().verticalGuides(), expectedVertical);
    const SelectionMask::Snapshot restoredSelection =
        first.document().selectionMask().snapshot();
    QCOMPARE(restoredSelection.size, expectedSelection.size);
    QCOMPARE(restoredSelection.active, expectedSelection.active);
    QCOMPARE(restoredSelection.implicitCoverage, expectedSelection.implicitCoverage);
    QVERIFY(restoredSelection.tiles == expectedSelection.tiles);
    QCOMPARE(restoredSelection.nonZeroBounds, expectedSelection.nonZeroBounds);
    QCOMPARE(first.document().layers().size(), expectedLayers.size());
    for (qsizetype index = 0; index < expectedLayers.size(); ++index) {
        const LayerNode &actual = first.document().layers().at(index);
        const LayerNode &expected = expectedLayers.at(index);
        QCOMPARE(actual.id, expected.id);
        QCOMPARE(actual.rasterReferenceSize, expected.rasterReferenceSize);
        QCOMPARE(actual.rasterReferenceOrigin, expected.rasterReferenceOrigin);
        QCOMPARE(actual.maskReferenceSize, expected.maskReferenceSize);
        QCOMPARE(actual.maskReferenceOrigin, expected.maskReferenceOrigin);
        QVERIFY(transformsClose(actual.transform, expected.transform));
        QVERIFY(exactImagesEqual(actual.rasterImage, expected.rasterImage));
        QVERIFY(exactImagesEqual(actual.maskImage, expected.maskImage));
    }
}

void CoreTests::renderedExportPreservesResolutionMetadata()
{
    QImage source(QSize(4, 3), QImage::Format_RGBA8888);
    source.fill(QColor(30, 60, 90, 180));
    source.setDotsPerMeterX(9449);
    source.setDotsPerMeterY(7087);
    PhotoDocument document;
    document.setSourceImage(source);

    const QImage rendered = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr, source.size());
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.dotsPerMeterX(), document.sourceImage().dotsPerMeterX());
    QCOMPARE(rendered.dotsPerMeterY(), document.sourceImage().dotsPerMeterY());
}

void CoreTests::canvasBoundsStateSurvivesColdResidency()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    DocumentResidencyManager::Limits limits;
    limits.residentDocumentBytes = 64LL * 1024LL * 1024LL;
    limits.warmSessionCount = 0;
    DocumentResidencyManager manager(limits, temporaryDirectory.path());

    DocumentSession first;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Canvas Bounds Cold State");
    settings.pixelSize = QSize(9, 7);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(12, 34, 56, 0);
    QString error;
    QVERIFY2(first.document().createNewDocument(settings, &error), qPrintable(error));

    const QUuid baseId = first.document().baseLayerId();
    QImage basePixels(9, 7, QImage::Format_RGBA64);
    for (int y = 0; y < basePixels.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(basePixels.scanLine(y));
        for (int x = 0; x < basePixels.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(1000 + x * 31),
                static_cast<quint16>(2000 + y * 47),
                static_cast<quint16>(3000 + (x + y) * 19),
                static_cast<quint16>((x + y) % 4 == 0 ? 0 : 65535));
        }
    }
    QVERIFY(first.document().updateLayer(baseId, [&](LayerNode &layer) {
        layer.rasterImage = basePixels;
        layer.rasterReferenceSize = basePixels.size();
        layer.rasterReferenceOrigin = QPointF(-2.0, 1.0);
    }));
    first.document().selectionMask().selectNone();
    QVERIFY(first.document().selectionMask().setCoverageRect(QRect(2, 1, 4, 3), 91));
    first.document().setGuides({1.0, 5.5}, {2.0, 7.0});

    CanvasSizeRequest request;
    request.pixelSize = QSize(15, 12);
    request.anchor = CanvasAnchor::Centre;
    request.fillMode = CanvasFillMode::Custom;
    request.fillColour = QColor(73, 41, 199, 0);
    CanvasSizeResult prepared;
    QVERIFY2(buildCanvasSizeResult(first.document(), request, &prepared, nullptr, &error),
             qPrintable(error));
    QVERIFY(prepared.extensionLayerCreated);
    QVERIFY2(first.document().replaceStructuralState(prepared.canvasImage,
                                                     prepared.layers,
                                                     prepared.selection,
                                                     prepared.horizontalGuides,
                                                     prepared.verticalGuides,
                                                     &error),
             qPrintable(error));
    first.refreshSummary();

    const QImage expectedCanvas = first.document().sourceImage();
    const SelectionMask::Snapshot expectedSelection = first.document().selectionMask().snapshot();
    const QVector<double> expectedHorizontal = first.document().horizontalGuides();
    const QVector<double> expectedVertical = first.document().verticalGuides();
    const QVector<LayerNode> expectedLayers = first.document().layers();
    QCOMPARE(expectedLayers.size(), 2);

    DocumentSession second;
    settings.name = QStringLiteral("Eviction Driver");
    settings.pixelSize = QSize(8, 8);
    settings.bitDepth = 8;
    QVERIFY2(second.document().createNewDocument(settings, &error), qPrintable(error));

    manager.registerSession(&first);
    manager.registerSession(&second);
    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY2(manager.activateSession(&second, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Cold);
    QVERIFY(!first.document().hasImage());

    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Hot);
    QVERIFY(exactImagesEqual(first.document().sourceImage(), expectedCanvas));
    const SelectionMask::Snapshot restoredSelection =
        first.document().selectionMask().snapshot();
    QCOMPARE(restoredSelection.size, expectedSelection.size);
    QCOMPARE(restoredSelection.active, expectedSelection.active);
    QCOMPARE(restoredSelection.implicitCoverage,
             expectedSelection.implicitCoverage);
    QVERIFY(restoredSelection.tiles == expectedSelection.tiles);
    QCOMPARE(restoredSelection.nonZeroBounds, expectedSelection.nonZeroBounds);
    QCOMPARE(first.document().horizontalGuides(), expectedHorizontal);
    QCOMPARE(first.document().verticalGuides(), expectedVertical);
    QCOMPARE(first.document().layers().size(), expectedLayers.size());
    for (qsizetype index = 0; index < expectedLayers.size(); ++index) {
        const LayerNode &expected = expectedLayers.at(index);
        const LayerNode &actual = first.document().layers().at(index);
        QCOMPARE(actual.id, expected.id);
        QCOMPARE(actual.name, expected.name);
        QVERIFY(actual.type == expected.type);
        QCOMPARE(actual.rasterReferenceSize, expected.rasterReferenceSize);
        QCOMPARE(actual.rasterReferenceOrigin, expected.rasterReferenceOrigin);
        QVERIFY(transformsClose(actual.transform, expected.transform));
        QVERIFY(exactImagesEqual(actual.rasterImage, expected.rasterImage));
        QVERIFY(exactImagesEqual(actual.maskImage, expected.maskImage));
    }
    const LayerNode extension = first.document().layers().constLast();
    const QRgba64 hiddenFill = reinterpret_cast<const QRgba64 *>(
        extension.rasterImage.constScanLine(0))[0];
    QCOMPARE(hiddenFill.alpha(), quint16(0));
    QVERIFY(hiddenFill.red() != 0 || hiddenFill.green() != 0
            || hiddenFill.blue() != 0);
}

void CoreTests::revealAllIncludesHiddenStraightRgbaStorageAndNeverShrinks()
{
    QImage source(4, 4, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("reveal-all.tga"));
    const QUuid baseId = document.baseLayerId();

    QImage storage(6, 5, QImage::Format_RGBA8888);
    storage.fill(Qt::transparent);
    uchar *hidden = storage.scanLine(0);
    hidden[0] = 91;
    hidden[1] = 37;
    hidden[2] = 203;
    hidden[3] = 0;
    uchar *visible = storage.scanLine(4) + 5 * 4;
    visible[0] = 10;
    visible[1] = 20;
    visible[2] = 30;
    visible[3] = 255;
    QVERIFY(document.updateLayer(baseId, [&storage](LayerNode &layer) {
        layer.rasterImage = storage;
        layer.rasterReferenceSize = storage.size();
        layer.rasterReferenceOrigin = QPointF(-2.0, -1.0);
        layer.transform = QTransform::fromTranslate(1.0, 2.0);
        layer.visible = false;
        layer.opacity = 0.0;
        layer.maskImage = QImage(1, 1, QImage::Format_Grayscale8);
        layer.maskImage.fill(0);
        layer.maskReferenceSize = storage.size();
        layer.maskReferenceOrigin = QPointF(-2.0, -1.0);
    }));

    CanvasFitRequest request;
    request.mode = CanvasFitMode::RevealAll;
    CanvasFitResult result;
    QString error;
    QVERIFY2(buildCanvasFitResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(!result.noChange);
    QCOMPARE(result.documentRect, QRect(-1, 0, 6, 6));
    QCOMPARE(result.canvas.canvasImage.size(), QSize(6, 6));
    QCOMPARE(result.canvas.layers.size(), 1);
    const LayerNode &revealed = result.canvas.layers.constFirst();
    QCOMPARE(revealed.rasterImage, storage);
    QCOMPARE(revealed.rasterReferenceOrigin, QPointF(-2.0, -1.0));
    QCOMPARE(revealed.transform, QTransform::fromTranslate(2.0, 2.0));
}

void CoreTests::fitCanvasToSelectedVectorUsesSemanticBounds()
{
    QImage source(10, 10, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("fit-vector.tga"));
    const QUuid vectorId = document.addVectorShape(
        VectorShapeType::RoundedRectangle,
        QRectF(-5.0, 2.0, 4.0, 3.0), QColor(80, 160, 220, 200), {}, 1.0);
    QVERIFY(!vectorId.isNull());
    QVERIFY(document.updateLayer(vectorId, [](LayerNode &layer) {
        layer.visible = false;
        layer.opacity = 0.0;
        layer.transform = QTransform::fromTranslate(2.0, 1.0);
    }));

    CanvasFitRequest request;
    request.mode = CanvasFitMode::SelectedLayers;
    request.layerIds = {vectorId};
    CanvasFitResult result;
    QString error;
    QVERIFY2(buildCanvasFitResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(!result.noChange);
    QCOMPARE(result.documentRect, QRect(-3, 3, 4, 3));
    QCOMPARE(result.canvas.canvasImage.size(), QSize(4, 3));

    const LayerNode fitted = result.canvas.layers.constLast();
    QCOMPARE(fitted.id, vectorId);
    QCOMPARE(fitted.type, LayerType::Vector);
    QCOMPARE(fitted.vectorData, document.layerById(vectorId).vectorData);
    QCOMPARE(fitted.transform, QTransform::fromTranslate(5.0, -2.0));
    QVERIFY(fitted.rasterImage.isNull());
}

void CoreTests::fitCanvasToSelectionUsesExactNonZeroCoverageBounds()
{
    QImage source(8, 7, QImage::Format_RGBA8888);
    source.fill(QColor(20, 30, 40, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("fit-selection.tga"));
    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(2, 1, 4, 3), 192));
    document.setGuides({1.0, 2.0, 4.0}, {2.0, 5.0, 7.0});

    CanvasFitRequest request;
    request.mode = CanvasFitMode::Selection;
    CanvasFitResult result;
    QString error;
    QVERIFY2(buildCanvasFitResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.documentRect, QRect(2, 1, 4, 3));
    QCOMPARE(result.canvas.canvasImage.size(), QSize(4, 3));
    QCOMPARE(result.canvas.layers.size(), 1);
    QCOMPARE(result.canvas.layers.constFirst().transform,
             QTransform::fromTranslate(-2.0, -1.0));

    SelectionMask fitted;
    QVERIFY(fitted.restoreSnapshot(result.canvas.selection, false));
    QVERIFY(fitted.isActive());
    QCOMPARE(fitted.nonZeroBounds(), QRect(0, 0, 4, 3));
    QCOMPARE(fitted.coverageAt(0, 0), static_cast<quint8>(192));
    QCOMPARE(result.canvas.horizontalGuides, QVector<double>({0.0, 1.0, 3.0}));
    QCOMPARE(result.canvas.verticalGuides, QVector<double>({0.0, 3.0}));
}

void CoreTests::fitCanvasToSelectedHiddenGroupUsesFiniteTransformedBounds()
{
    QImage source(10, 10, QImage::Format_RGBA64);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("fit-hidden-group.tga"));
    const QUuid groupId = document.addGroup();
    const QUuid childId = document.addRasterLayer(groupId);
    QVERIFY(!groupId.isNull());
    QVERIFY(!childId.isNull());

    QImage childPixels(2, 3, QImage::Format_RGBA64);
    childPixels.fill(Qt::transparent);
    auto *row = reinterpret_cast<QRgba64 *>(childPixels.scanLine(1));
    row[1] = QRgba64::fromRgba64(32000, 12000, 50000, 1);
    QVERIFY(document.updateLayer(groupId, [](LayerNode &group) {
        group.visible = false;
        group.opacity = 0.0;
        group.transform = QTransform::fromTranslate(-4.0, 5.0);
    }));
    QVERIFY(document.updateLayer(childId, [&childPixels](LayerNode &child) {
        child.visible = false;
        child.opacity = 0.0;
        child.rasterImage = childPixels;
        child.rasterReferenceSize = childPixels.size();
        child.rasterReferenceOrigin = QPointF(1.0, -1.0);
        child.transform = QTransform::fromTranslate(2.0, 1.0);
    }));

    CanvasFitRequest request;
    request.mode = CanvasFitMode::SelectedLayers;
    request.layerIds = {groupId};
    CanvasFitResult result;
    QString error;
    QVERIFY2(buildCanvasFitResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(!result.noChange);
    // The single alpha=1 16-bit pixel is at local [2,0] and therefore world
    // [-0,6] after the child and group translations.
    QCOMPARE(result.documentRect, QRect(0, 6, 1, 1));
    QCOMPARE(result.canvas.canvasImage.size(), QSize(1, 1));
    const LayerNode fittedGroup = result.canvas.layers.constFirst();
    QCOMPARE(fittedGroup.id, groupId);
    QCOMPARE(fittedGroup.transform, QTransform::fromTranslate(-4.0, -1.0));
    QCOMPARE(fittedGroup.children.constFirst().transform,
             QTransform::fromTranslate(2.0, 1.0));
}

void CoreTests::fitCanvasToMaskedAdjustmentAndIgnoresUnboundedAdjustment()
{
    QImage source(12, 9, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("fit-adjustment.tga"));
    const QUuid adjustmentId = document.addAdjustment(AdjustmentType::Exposure);
    QVERIFY(document.addMask(adjustmentId));

    QImage mask(5, 4, QImage::Format_Grayscale8);
    mask.fill(0);
    mask.scanLine(2)[1] = 64;
    mask.scanLine(2)[2] = 255;
    QVERIFY(document.updateLayer(adjustmentId, [&mask](LayerNode &layer) {
        layer.maskImage = mask;
        layer.maskReferenceSize = mask.size();
        layer.maskReferenceOrigin = QPointF(-3.0, 4.0);
        layer.transform = QTransform::fromTranslate(2.0, -1.0);
        layer.visible = false;
    }));

    CanvasFitRequest masked;
    masked.mode = CanvasFitMode::SelectedLayers;
    masked.layerIds = {adjustmentId};
    CanvasFitResult result;
    QString error;
    QVERIFY2(buildCanvasFitResult(document, masked, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.documentRect, QRect(0, 5, 2, 1));

    const QUuid unboundedId = document.addAdjustment(AdjustmentType::Contrast);
    CanvasFitRequest unbounded;
    unbounded.mode = CanvasFitMode::SelectedLayers;
    unbounded.layerIds = {unboundedId};
    CanvasFitResult noBounds;
    QVERIFY2(buildCanvasFitResult(document, unbounded, &noBounds, nullptr, &error),
             qPrintable(error));
    QVERIFY(noBounds.noChange);
    QVERIFY(noBounds.canvas.canvasImage.isNull());
    QVERIFY(!noBounds.noChangeMessage.isEmpty());
}

void CoreTests::trimTransparentUsesVisibleCompositeAlphaAndPreservesStorage()
{
    QImage source(7, 6, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(30 + x * 7);
            row[x * 4 + 1] = static_cast<uchar>(40 + y * 9);
            row[x * 4 + 2] = static_cast<uchar>(50 + x + y);
            row[x * 4 + 3] = (x >= 2 && x <= 5 && y >= 1 && y <= 3)
                ? 255 : 0;
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("trim-alpha.tga"));
    const QUuid baseId = document.baseLayerId();

    // A hidden opaque layer must not affect visible-composite trim bounds.
    const QUuid hiddenId = document.addRasterLayer();
    QImage hidden(7, 6, QImage::Format_RGBA8888);
    hidden.fill(QColor(255, 0, 0, 255));
    QVERIFY(document.updateLayer(hiddenId, [&hidden](LayerNode &layer) {
        layer.rasterImage = hidden;
        layer.rasterReferenceSize = hidden.size();
        layer.visible = false;
    }));

    AutomaticTrimRequest request;
    request.mode = AutomaticTrimMode::TransparentPixels;
    AutomaticTrimResult result;
    QString error;
    QVERIFY2(buildAutomaticTrimResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QVERIFY(!result.noChange);
    QCOMPARE(result.documentRect, QRect(2, 1, 4, 3));
    QCOMPARE(result.canvas.canvasImage.size(), QSize(4, 3));
    QCOMPARE(result.canvas.layers.size(), 2);

    const auto baseIterator = std::find_if(
        result.canvas.layers.cbegin(),
        result.canvas.layers.cend(),
        [baseId](const LayerNode &layer) { return layer.id == baseId; });
    QVERIFY(baseIterator != result.canvas.layers.cend());
    const LayerNode &trimmedBase = *baseIterator;
    QVERIFY(exactImagesEqual(trimmedBase.rasterImage, source));
    QCOMPARE(trimmedBase.rasterReferenceSize, source.size());
    QCOMPARE(trimmedBase.transform, QTransform::fromTranslate(-2.0, -1.0));
    QVERIFY(!result.canvas.destructiveClippingApplied);
}

void CoreTests::trimTransparentCountsSixteenBitAlphaOne()
{
    QImage source(5, 4, QImage::Format_RGBA64);
    source.fill(Qt::transparent);
    auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(2));
    row[3] = QRgba64::fromRgba64(1234, 2345, 3456, 1);

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("trim-alpha-one.tiff"));
    AutomaticTrimRequest request;
    request.mode = AutomaticTrimMode::TransparentPixels;
    AutomaticTrimResult result;
    QString error;
    QVERIFY2(buildAutomaticTrimResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.documentRect, QRect(3, 2, 1, 1));
    QCOMPARE(result.canvas.canvasImage.format(), QImage::Format_RGBA64);
    QCOMPARE(result.canvas.canvasImage.size(), QSize(1, 1));

    QImage transparent(4, 3, QImage::Format_RGBA64);
    transparent.fill(Qt::transparent);
    PhotoDocument emptyDocument;
    emptyDocument.setSourceImage(transparent, QStringLiteral("empty-trim.tiff"));
    AutomaticTrimResult empty;
    QVERIFY2(buildAutomaticTrimResult(emptyDocument,
                                      request,
                                      &empty,
                                      nullptr,
                                      &error),
             qPrintable(error));
    QVERIFY(empty.noChange);
    QVERIFY(empty.canvas.canvasImage.isNull());
}

void CoreTests::trimTransparentHonoursPassThroughGroupMasks()
{
    QImage source(8, 7, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("trim-group-mask.tga"));

    const QUuid rasterId = document.addRasterLayer();
    QImage pixels(8, 7, QImage::Format_RGBA8888);
    pixels.fill(QColor(70, 120, 180, 255));
    QVERIFY(document.updateLayer(rasterId, [&pixels](LayerNode &layer) {
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = pixels.size();
    }));
    const QUuid groupId = document.groupLayers(
        {rasterId}, QStringLiteral("Masked Pass Through"));
    QVERIFY(!groupId.isNull());
    QVERIFY(document.addMask(groupId));

    QImage mask(8, 7, QImage::Format_Grayscale8);
    mask.fill(0);
    for (int y = 2; y <= 5; ++y) {
        std::fill(mask.scanLine(y) + 1,
                  mask.scanLine(y) + 6,
                  static_cast<uchar>(255));
    }
    QVERIFY(document.updateLayer(groupId, [&mask](LayerNode &layer) {
        layer.groupCompositeMode = GroupCompositeMode::PassThrough;
        layer.maskImage = mask;
        layer.maskReferenceSize = mask.size();
        layer.maskReferenceOrigin = QPointF();
        layer.maskEnabled = true;
    }));

    AutomaticTrimRequest request;
    request.mode = AutomaticTrimMode::TransparentPixels;
    AutomaticTrimResult result;
    QString error;
    QVERIFY2(buildAutomaticTrimResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.documentRect, QRect(1, 2, 5, 4));
}

void CoreTests::trimCornerColourUsesToleranceSidesAndTransparentEquivalence()
{
    QImage source(8, 6, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            // Deliberately vary hidden RGB around the transparent border.
            row[x * 4 + 0] = static_cast<uchar>(5 + x * 13);
            row[x * 4 + 1] = static_cast<uchar>(11 + y * 17);
            row[x * 4 + 2] = static_cast<uchar>(23 + x + y);
            row[x * 4 + 3] = 0;
        }
    }
    for (int y = 2; y <= 4; ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 3; x <= 6; ++x) {
            row[x * 4 + 0] = 100;
            row[x * 4 + 1] = 110;
            row[x * 4 + 2] = 120;
            row[x * 4 + 3] = 255;
        }
    }

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("trim-transparent-corner.tga"));
    AutomaticTrimRequest request;
    request.mode = AutomaticTrimMode::CornerColour;
    request.sampleCorner = TrimSampleCorner::TopLeft;
    request.tolerance = 0;

    AutomaticTrimResult allSides;
    QString error;
    QVERIFY2(buildAutomaticTrimResult(document,
                                      request,
                                      &allSides,
                                      nullptr,
                                      &error),
             qPrintable(error));
    QVERIFY(allSides.sampledColourValid);
    QCOMPARE(allSides.sampledColour.alpha(), 0);
    QCOMPARE(allSides.documentRect, QRect(3, 2, 4, 3));

    request.trimLeft = false;
    request.trimBottom = false;
    AutomaticTrimResult selectedSides;
    QVERIFY2(buildAutomaticTrimResult(document,
                                      request,
                                      &selectedSides,
                                      nullptr,
                                      &error),
             qPrintable(error));
    QCOMPARE(selectedSides.documentRect, QRect(0, 2, 7, 4));

    QImage toleranceImage(6, 5, QImage::Format_RGBA8888);
    toleranceImage.fill(QColor(20, 30, 40, 255));
    toleranceImage.setPixelColor(5, 0, QColor(22, 32, 42, 253));
    for (int y = 1; y <= 3; ++y) {
        for (int x = 1; x <= 4; ++x) {
            toleranceImage.setPixelColor(x, y, QColor(80, 90, 100, 255));
        }
    }
    PhotoDocument toleranceDocument;
    toleranceDocument.setSourceImage(toleranceImage,
                                     QStringLiteral("trim-tolerance.tga"));
    AutomaticTrimRequest toleranceRequest;
    toleranceRequest.mode = AutomaticTrimMode::CornerColour;
    toleranceRequest.sampleCorner = TrimSampleCorner::TopLeft;
    toleranceRequest.tolerance = 2;
    AutomaticTrimResult toleranceResult;
    QVERIFY2(buildAutomaticTrimResult(toleranceDocument,
                                      toleranceRequest,
                                      &toleranceResult,
                                      nullptr,
                                      &error),
             qPrintable(error));
    QCOMPARE(toleranceResult.documentRect, QRect(1, 1, 4, 3));
}

void CoreTests::trimCornerColourDestructiveModeClipsExactStorage()
{
    QImage source(6, 5, QImage::Format_RGBA8888);
    source.fill(QColor(10, 20, 30, 255));
    for (int y = 1; y <= 3; ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 2; x <= 4; ++x) {
            row[x * 4 + 0] = static_cast<uchar>(100 + x);
            row[x * 4 + 1] = static_cast<uchar>(110 + y);
            row[x * 4 + 2] = static_cast<uchar>(120 + x + y);
            row[x * 4 + 3] = (x == 3 && y == 2) ? 0 : 200;
        }
    }
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("trim-destructive.tga"));

    AutomaticTrimRequest request;
    request.mode = AutomaticTrimMode::CornerColour;
    request.sampleCorner = TrimSampleCorner::TopLeft;
    request.deleteOutsideCanvas = true;
    AutomaticTrimResult result;
    QString error;
    QVERIFY2(buildAutomaticTrimResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.documentRect, QRect(2, 1, 3, 3));
    QVERIFY(result.canvas.destructiveClippingApplied);
    QCOMPARE(result.canvas.layers.size(), 1);
    const LayerNode &layer = result.canvas.layers.constFirst();
    QCOMPARE(layer.rasterImage.size(), QSize(3, 3));
    QCOMPARE(layer.rasterReferenceSize, QSize(3, 3));
    QCOMPARE(layer.rasterReferenceOrigin, QPointF());
    QVERIFY(layer.transform.isIdentity());

    const uchar *hidden = layer.rasterImage.constScanLine(1) + 4;
    QCOMPARE(hidden[3], static_cast<uchar>(0));
    QCOMPARE(hidden[0], static_cast<uchar>(103));
    QCOMPARE(hidden[1], static_cast<uchar>(112));
    QCOMPARE(hidden[2], static_cast<uchar>(125));
}

void CoreTests::automaticTrimCancellationPublishesNoResult()
{
    QImage source(32, 32, QImage::Format_RGBA8888);
    source.fill(QColor(20, 40, 60, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("trim-cancel.tga"));

    std::atomic_bool cancelRequested(true);
    AutomaticTrimRequest request;
    request.mode = AutomaticTrimMode::TransparentPixels;
    AutomaticTrimResult result;
    QString error;
    QVERIFY(!buildAutomaticTrimResult(document,
                                      request,
                                      &result,
                                      &cancelRequested,
                                      &error));
    QVERIFY(result.canvas.canvasImage.isNull());
    QVERIFY(!error.isEmpty());
}

void CoreTests::legacyBaseImagePromotesToEditableRaster()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(3, 2, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    uchar *row = source.scanLine(0);
    row[0] = 211;
    row[1] = 73;
    row[2] = 149;
    row[3] = 0;

    PhotoDocument original;
    original.setSourceImage(source, QStringLiteral("legacy-source.tga"));
    const QUuid originalId = original.baseLayerId();
    const QString path = directory.filePath(QStringLiteral("legacy-base.vfxphoto"));
    QString error;
    QVERIFY2(original.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(json.isObject());
    QJsonObject root = json.object();
    root.remove(QStringLiteral("editableRasterBase"));
    root.remove(QStringLiteral("sourceRasterLayerId"));
    QJsonArray layers = root.value(QStringLiteral("layerTree")).toArray();
    QVERIFY(!layers.isEmpty());
    QJsonObject legacy = layers.first().toObject();
    legacy.insert(QStringLiteral("kind"), QStringLiteral("base-image"));
    legacy.remove(QStringLiteral("rasterEncoding"));
    legacy.remove(QStringLiteral("rasterData"));
    layers[0] = legacy;
    root.insert(QStringLiteral("layerTree"), layers);

    const QByteArray legacyBytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(legacyBytes), static_cast<qint64>(legacyBytes.size()));
    file.close();

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QCOMPARE(loaded.baseLayerId(), originalId);
    const LayerNode promoted = loaded.layerById(originalId);
    QVERIFY(promoted.type == LayerType::Raster);
    QCOMPARE(promoted.name, QStringLiteral("Base Image — legacy-source.tga"));
    QCOMPARE(promoted.rasterImage.convertToFormat(QImage::Format_RGBA8888), source);
    QVERIFY(!loaded.isModified());
}

void CoreTests::emptyEditableLayerTreeRoundTrips()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(17, 11);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.removeLayer(document.baseLayerId()));
    QCOMPARE(document.layerCount(), 0);
    QVERIFY(document.baseLayerId().isNull());

    const QString path = directory.filePath(QStringLiteral("empty-layers.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(loaded.hasImage());
    QCOMPARE(loaded.sourceImage().size(), QSize(17, 11));
    QCOMPARE(loaded.layerCount(), 0);
    QVERIFY(loaded.baseLayerId().isNull());
    QVERIFY(!loaded.isModified());
}

void CoreTests::newDocumentGrayscale16RoundTripsSettings()
{
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Height Working File");
    settings.pixelSize = QSize(37, 23);
    settings.bitDepth = 16;
    settings.colourModel = DocumentColourModel::Grayscale;
    settings.colourSpace = QColorSpace(QColorSpace::SRgbLinear);
    settings.backgroundColour = QColor(200, 40, 100, 77);
    settings.resolutionX = 300.0;
    settings.resolutionY = 300.0;

    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.sourceImage().depth() > 32);
    QCOMPARE(document.sourceImage().format(), QImage::Format_RGBA64);
    const QColor pixel = document.sourceImage().pixelColor(3, 4);
    QCOMPARE(pixel.red(), pixel.green());
    QCOMPARE(pixel.green(), pixel.blue());
    QCOMPARE(pixel.red(), qGray(settings.backgroundColour.rgb()));
    QCOMPARE(pixel.alpha(), 77);
    QVERIFY(document.colourModel() == DocumentColourModel::Grayscale);
    QCOMPARE(document.colourProfileName(), QStringLiteral("Linear sRGB"));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("new-document.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(json.isObject());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(),
             PhotoDocument::ProjectFormatVersion);
    QCOMPARE(PhotoDocument::ProjectFormatVersion, 27);
    const QJsonObject savedSettings =
        json.object().value(QStringLiteral("documentSettings")).toObject();
    QCOMPARE(savedSettings.value(QStringLiteral("name")).toString(), settings.name);
    QCOMPARE(savedSettings.value(QStringLiteral("colourModel")).toString(),
             QStringLiteral("grayscale"));
    QCOMPARE(savedSettings.value(QStringLiteral("colourSpace")).toString(),
             QStringLiteral("linear-srgb"));
    QVERIFY(savedSettings.value(QStringLiteral("blankDocument")).toBool());

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(loaded.isBlankDocument());
    QCOMPARE(loaded.documentName(), settings.name);
    QVERIFY(loaded.colourModel() == DocumentColourModel::Grayscale);
    QCOMPARE(loaded.colourProfileName(), QStringLiteral("Linear sRGB"));
    QVERIFY(loaded.sourceImage().depth() > 32);
    QCOMPARE(loaded.sourceImage().pixelColor(3, 4), pixel);
    QVERIFY(std::abs(loaded.resolutionX() - 300.0) < 0.001);
    QVERIFY(std::abs(loaded.resolutionY() - 300.0) < 0.001);
    QCOMPARE(loaded.layerById(loaded.baseLayerId()).name, QStringLiteral("Background"));
}

void CoreTests::newDocumentRejectsInvalidDimensions()
{
    NewDocumentSettings settings;
    settings.pixelSize = QSize(0, 64);
    PhotoDocument document;
    QString error;
    QVERIFY(!document.createNewDocument(settings, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!document.hasImage());
}

void CoreTests::exposureBrightensImage()
{
    QImage source(4, 4, QImage::Format_RGBA8888);
    source.fill(QColor(64, 64, 64, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.name = QStringLiteral("Base");

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.name = QStringLiteral("Exposure");
    exposure.exposure = 1.0;

    const QImage result = ImageProcessor::render(source, {exposure, base});
    QVERIFY(!result.isNull());
    QVERIFY(result.pixelColor(0, 0).red() > source.pixelColor(0, 0).red());
}


void CoreTests::adjustmentPreservesCoverage()
{
    QImage source(3, 3, QImage::Format_RGBA8888);
    source.fill(QColor(64, 64, 64, 128));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 1.0;

    const QImage result = ImageProcessor::render(source, {exposure, base});
    QVERIFY(!result.isNull());
    QCOMPARE(result.pixelColor(1, 1).alpha(), 128);
}

void CoreTests::hiddenBaseRevealsTransparency()
{
    QImage source(3, 3, QImage::Format_RGBA8888);
    source.fill(QColor(255, 0, 0, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.visible = false;

    const QImage result = ImageProcessor::render(source, {base});
    QVERIFY(!result.isNull());
    QCOMPARE(result.pixelColor(1, 1).alpha(), 0);
}


void CoreTests::multiplyRasterLayerBlends()
{
    QImage source(2, 2, QImage::Format_RGBA8888);
    source.fill(QColor(100, 100, 100, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.blendMode = BlendMode::Multiply;
    raster.rasterImage = QImage(2, 2, QImage::Format_RGBA8888);
    raster.rasterImage.fill(QColor(128, 255, 128, 255));

    const QImage result = ImageProcessor::render(source, {raster, base});
    QVERIFY(!result.isNull());
    const QColor pixel = result.pixelColor(0, 0);
    QVERIFY(pixel.red() >= 49 && pixel.red() <= 51);
    QVERIFY(pixel.green() >= 99 && pixel.green() <= 101);
    QVERIFY(pixel.blue() >= 49 && pixel.blue() <= 51);
    QCOMPARE(pixel.alpha(), 255);
}


void CoreTests::translatedRasterLayerMoves()
{
    QImage source(6, 4, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);

    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.rasterImage = QImage(6, 4, QImage::Format_RGBA8888);
    raster.rasterImage.fill(Qt::transparent);
    raster.rasterImage.setPixelColor(1, 1, QColor(255, 0, 0, 255));
    raster.transform = QTransform::fromTranslate(2.0, 1.0);

    const QImage result = ImageProcessor::render(source, {raster}, nullptr, source.size());
    QVERIFY(!result.isNull());
    QCOMPARE(result.pixelColor(1, 1).alpha(), 0);
    QCOMPARE(result.pixelColor(3, 2), QColor(255, 0, 0, 255));
}

void CoreTests::maskLifecycleSupportsRasterAdjustmentAndGroupLayers()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.fill(QColor(100, 20, 20, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    QImage halfMask(2, 1, QImage::Format_Grayscale8);
    halfMask.scanLine(0)[0] = 255;
    halfMask.scanLine(0)[1] = 0;

    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.rasterImage = QImage(source.size(), QImage::Format_RGBA8888);
    raster.rasterImage.fill(QColor(20, 40, 220, 255));
    raster.maskImage = halfMask;

    QImage result = ImageProcessor::render(source, {raster, base});
    QCOMPARE(result.pixelColor(0, 0), QColor(20, 40, 220, 255));
    QCOMPARE(result.pixelColor(1, 0), QColor(100, 20, 20, 255));

    raster.maskEnabled = false;
    result = ImageProcessor::render(source, {raster, base});
    QCOMPARE(result.pixelColor(0, 0), QColor(20, 40, 220, 255));
    QCOMPARE(result.pixelColor(1, 0), QColor(20, 40, 220, 255));

    raster.maskEnabled = true;
    raster.maskInverted = true;
    result = ImageProcessor::render(source, {raster, base});
    QCOMPARE(result.pixelColor(0, 0), QColor(100, 20, 20, 255));
    QCOMPARE(result.pixelColor(1, 0), QColor(20, 40, 220, 255));

    QImage compactWhiteMask(1, 1, QImage::Format_Grayscale8);
    compactWhiteMask.fill(255);
    raster.maskImage = compactWhiteMask;
    raster.maskInverted = true;
    result = ImageProcessor::render(source, {raster, base});
    QCOMPARE(result, source);
    raster.maskEnabled = false;
    result = ImageProcessor::render(source, {raster, base});
    QCOMPARE(result.pixelColor(0, 0), QColor(20, 40, 220, 255));
    QCOMPARE(result.pixelColor(1, 0), QColor(20, 40, 220, 255));
    raster.maskEnabled = true;
    raster.maskImage = halfMask;

    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    adjustment.adjustmentType = AdjustmentType::Exposure;
    adjustment.exposure = 1.0;
    adjustment.maskImage = halfMask;
    result = ImageProcessor::render(source, {adjustment, base});
    QVERIFY(result.pixelColor(0, 0).red() > source.pixelColor(0, 0).red());
    QCOMPARE(result.pixelColor(1, 0), source.pixelColor(1, 0));

    adjustment.maskInverted = true;
    result = ImageProcessor::render(source, {adjustment, base});
    QCOMPARE(result.pixelColor(0, 0), source.pixelColor(0, 0));
    QVERIFY(result.pixelColor(1, 0).red() > source.pixelColor(1, 0).red());

    LayerNode child = raster;
    child.maskImage = {};
    child.maskEnabled = true;
    child.maskInverted = false;
    LayerNode group;
    group.type = LayerType::Group;
    group.children = {child};
    group.maskImage = halfMask;
    result = ImageProcessor::render(source, {group, base});
    QCOMPARE(result.pixelColor(0, 0), QColor(20, 40, 220, 255));
    QCOMPARE(result.pixelColor(1, 0), QColor(100, 20, 20, 255));

    PhotoDocument document;
    document.setSourceImage(source);
    const QUuid rasterId = document.addRasterLayer(document.baseLayerId());
    QVERIFY(document.addMask(rasterId));
    QVERIFY(document.setMaskEnabled(rasterId, false));
    QVERIFY(document.setMaskInverted(rasterId, true));
    LayerNode stored = document.layerById(rasterId);
    QVERIFY(stored.hasMask());
    QVERIFY(!stored.maskEnabled);
    QVERIFY(stored.maskInverted);
    QVERIFY(document.removeMask(rasterId));
    stored = document.layerById(rasterId);
    QVERIFY(!stored.hasMask());
    QVERIFY(stored.maskEnabled);
    QVERIFY(!stored.maskInverted);
}


void CoreTests::dirtyRegionMatchesFullRender()
{
    QImage source(32, 24, QImage::Format_RGBA8888);
    source.fill(QColor(48, 96, 160, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.opacity = 0.65;
    raster.blendMode = BlendMode::Overlay;
    raster.rasterImage = QImage(source.size(), QImage::Format_RGBA8888);
    raster.rasterImage.fill(Qt::transparent);
    QPainter rasterPainter(&raster.rasterImage);
    rasterPainter.fillRect(QRect(5, 4, 18, 12), QColor(220, 80, 20, 200));

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 0.75;

    const QVector<LayerNode> layers {exposure, raster, base};
    const QRect region(7, 6, 11, 9);
    const QImage full = ImageProcessor::render(source, layers, nullptr, source.size());
    const QImage tile = ImageProcessor::renderRegion(source, layers, region, source.size());
    QVERIFY(!full.isNull());
    QVERIFY(!tile.isNull());
    QCOMPARE(tile.size(), region.size());
    for (int y = 0; y < tile.height(); ++y) {
        for (int x = 0; x < tile.width(); ++x) {
            QCOMPARE(tile.pixelColor(x, y), full.pixelColor(region.x() + x, region.y() + y));
        }
    }
}


void CoreTests::affineDirtyRegionMatchesFullRender()
{
    QImage source(64, 64, QImage::Format_RGBA8888);
    source.fill(QColor(35, 70, 105, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.opacity = 0.72;
    raster.blendMode = BlendMode::Overlay;
    raster.rasterImage = QImage(source.size(), QImage::Format_RGBA8888);
    raster.rasterImage.fill(Qt::transparent);
    QPainter painter(&raster.rasterImage);
    painter.fillRect(QRect(12, 14, 22, 16), QColor(230, 60, 25, 210));
    painter.end();

    QTransform rotate;
    rotate.rotate(23.0);
    QTransform scale;
    scale.scale(1.15, 0.8);
    raster.transform = QTransform::fromTranslate(-20.0, -20.0)
        * scale
        * rotate
        * QTransform::fromTranslate(28.0, 25.0);

    const QVector<LayerNode> layers {raster, base};
    const QRect region(8, 7, 38, 34);
    const QImage full = ImageProcessor::render(source, layers, nullptr, source.size());
    const QImage tile = ImageProcessor::renderRegion(source, layers, region, source.size());
    QVERIFY(!full.isNull());
    QVERIFY(!tile.isNull());
    QCOMPARE(tile.size(), region.size());

    for (int y = 0; y < tile.height(); ++y) {
        for (int x = 0; x < tile.width(); ++x) {
            const QColor actual = tile.pixelColor(x, y);
            const QColor expected = full.pixelColor(region.x() + x, region.y() + y);
            QVERIFY(std::abs(actual.red() - expected.red()) <= 1);
            QVERIFY(std::abs(actual.green() - expected.green()) <= 1);
            QVERIFY(std::abs(actual.blue() - expected.blue()) <= 1);
            QVERIFY(std::abs(actual.alpha() - expected.alpha()) <= 1);
        }
    }
}

void CoreTests::contentBoundsIgnoreTransparentAndMaskedPixels()
{
    QImage source(20, 20, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);

    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.rasterImage = QImage(source.size(), QImage::Format_RGBA8888);
    raster.rasterImage.fill(Qt::transparent);
    QPainter painter(&raster.rasterImage);
    painter.fillRect(QRect(5, 6, 4, 3), Qt::white);
    QImage mask(source.size(), QImage::Format_ARGB32);
    mask.fill(Qt::black);
    QPainter maskPainter(&mask);
    maskPainter.fillRect(QRect(6, 6, 2, 2), Qt::white);
    maskPainter.end();
    raster.maskImage = mask.convertToFormat(QImage::Format_Grayscale8);

    const QRectF bounds = ImageProcessor::contentBounds(source,
                                                         {raster},
                                                         {raster.id},
                                                         source.size());
    QCOMPARE(bounds, QRectF(6.0, 6.0, 2.0, 2.0));

    raster.maskImage.fill(0);
    QVERIFY(ImageProcessor::contentBounds(source,
                                          {raster},
                                          {raster.id},
                                          source.size()).isEmpty());
}

void CoreTests::passThroughGroupAdjustmentAffectsParent()
{
    QImage source(4, 3, QImage::Format_RGBA8888);
    source.fill(QColor(64, 64, 64, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 1.0;

    LayerNode group;
    group.type = LayerType::Group;
    group.children = {exposure};

    const QImage isolated = ImageProcessor::render(source, {group, base});
    QCOMPARE(isolated, source);

    group.groupCompositeMode = GroupCompositeMode::PassThrough;
    const QImage passThrough = ImageProcessor::render(source, {group, base});
    QVERIFY(!passThrough.isNull());
    QVERIFY(passThrough.pixelColor(1, 1).red() > source.pixelColor(1, 1).red());

    const QImage region = ImageProcessor::renderRegion(source,
                                                       {group, base},
                                                       source.rect(),
                                                       source.size());
    QCOMPARE(region, passThrough);
    const QRect partialRect(1, 1, 2, 1);
    QCOMPARE(ImageProcessor::renderRegion(source,
                                          {group, base},
                                          partialRect,
                                          source.size()),
             passThrough.copy(partialRect));
}

void CoreTests::passThroughGroupOpacityAndMaskMixBeforeAfterResults()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.fill(QColor(64, 64, 64, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 2.0;

    LayerNode fullGroup;
    fullGroup.type = LayerType::Group;
    fullGroup.groupCompositeMode = GroupCompositeMode::PassThrough;
    fullGroup.children = {exposure};
    const QImage fullEffect = ImageProcessor::render(source, {fullGroup, base});
    QVERIFY(fullEffect.pixelColor(0, 0).red() > source.pixelColor(0, 0).red());

    QImage halfMask(2, 1, QImage::Format_Grayscale8);
    halfMask.scanLine(0)[0] = 255;
    halfMask.scanLine(0)[1] = 0;

    LayerNode faded = fullGroup;
    faded.opacity = 0.5;
    faded.maskImage = halfMask;
    const QImage result = ImageProcessor::render(source, {faded, base});
    const int baseValue = source.pixelColor(0, 0).red();
    const int fullValue = fullEffect.pixelColor(0, 0).red();
    const int fadedValue = result.pixelColor(0, 0).red();
    QVERIFY(fadedValue > baseValue);
    QVERIFY(fadedValue < fullValue);
    QCOMPARE(result.pixelColor(1, 0), source.pixelColor(1, 0));

    faded.maskInverted = true;
    const QImage inverted = ImageProcessor::render(source, {faded, base});
    QCOMPARE(inverted.pixelColor(0, 0), source.pixelColor(0, 0));
    QVERIFY(inverted.pixelColor(1, 0).red() > source.pixelColor(1, 0).red());

    faded.maskEnabled = false;
    const QImage disabled = ImageProcessor::render(source, {faded, base});
    QVERIFY(disabled.pixelColor(0, 0).red() > source.pixelColor(0, 0).red());
    QVERIFY(disabled.pixelColor(1, 0).red() > source.pixelColor(1, 0).red());
}

void CoreTests::largePassThroughMixIsParallelSafeAndDeterministic()
{
    // Larger than ImageProcessor's parallel-row threshold. This specifically
    // exercises the Pass Through before/after mix on the dedicated processing
    // pool and guards against concurrent QImage implicit-detach corruption.
    const QSize size(512, 384);
    QImage source(size, QImage::Format_RGBA8888);
    QImage raster(size, QImage::Format_RGBA8888);
    QImage mask(size, QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        uchar *maskRow = mask.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            source.setPixelColor(x,
                                 y,
                                 QColor((19 + x * 3 + y) & 255,
                                        (47 + x + y * 5) & 255,
                                        (83 + x * 7 + y * 2) & 255,
                                        255));
            raster.setPixelColor(x,
                                 y,
                                 QColor((211 + x + y * 3) & 255,
                                        (37 + x * 5 + y) & 255,
                                        (129 + x * 2 + y * 7) & 255,
                                        (31 + x * 3 + y * 5) & 255));
            maskRow[x] = static_cast<uchar>((17 + x * 5 + y * 3) & 255);
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode child;
    child.type = LayerType::Raster;
    child.rasterImage = raster;
    child.opacity = 0.57;
    child.blendMode = BlendMode::Screen;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 0.75;

    LayerNode group;
    group.type = LayerType::Group;
    group.groupCompositeMode = GroupCompositeMode::PassThrough;
    group.children = {exposure, child};
    group.opacity = 0.61;
    group.maskImage = mask;

    const QVector<LayerNode> layers {group, base};
    const QImage first = ImageProcessor::render(source, layers);
    const QImage second = ImageProcessor::render(source, layers);
    QVERIFY(!first.isNull());
    QCOMPARE(second, first);
}

void CoreTests::nestedGroupModesPreserveIsolationBoundaries()
{
    QImage source(2, 2, QImage::Format_RGBA8888);
    source.fill(QColor(72, 72, 72, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 1.0;

    LayerNode inner;
    inner.type = LayerType::Group;
    inner.children = {exposure};

    LayerNode outer;
    outer.type = LayerType::Group;
    outer.children = {inner};

    // Pass Through cannot escape a containing isolated boundary.
    inner.groupCompositeMode = GroupCompositeMode::PassThrough;
    outer.groupCompositeMode = GroupCompositeMode::Isolated;
    QCOMPARE(ImageProcessor::render(source, {outer, base}), source);

    // An isolated child remains contained even when its parent passes through.
    inner.groupCompositeMode = GroupCompositeMode::Isolated;
    outer.groupCompositeMode = GroupCompositeMode::PassThrough;
    QCOMPARE(ImageProcessor::render(source, {outer, base}), source);

    // With both boundaries passing through, the adjustment reaches the base.
    inner.groupCompositeMode = GroupCompositeMode::PassThrough;
    const QImage result = ImageProcessor::render(source, {outer, base});
    QVERIFY(result.pixelColor(0, 0).red() > source.pixelColor(0, 0).red());
}

void CoreTests::deepNestedWorkflowRoundTripPreservesModesAndMasks()
{
    const QSize size(520, 310);
    QImage source(size, QImage::Format_RGBA8888);
    QImage raster(size, QImage::Format_RGBA8888);
    QImage outerMask(size, QImage::Format_Grayscale8);
    QImage innerMask(size, QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        uchar *outerRow = outerMask.scanLine(y);
        uchar *innerRow = innerMask.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            source.setPixelColor(x,
                                 y,
                                 QColor((29 + x * 3 + y) & 255,
                                        (67 + x + y * 5) & 255,
                                        (101 + x * 7 + y * 2) & 255,
                                        255));
            raster.setPixelColor(x,
                                 y,
                                 QColor((211 + x + y * 3) & 255,
                                        (43 + x * 5 + y) & 255,
                                        (137 + x * 2 + y * 7) & 255,
                                        (31 + x * 3 + y * 5) & 255));
            outerRow[x] = static_cast<uchar>((17 + x * 5 + y * 3) & 255);
            innerRow[x] = static_cast<uchar>((241 - x * 3 + y * 7) & 255);
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.name = QStringLiteral("Base");

    LayerNode deepestExposure;
    deepestExposure.type = LayerType::Adjustment;
    deepestExposure.adjustmentType = AdjustmentType::Exposure;
    deepestExposure.exposure = 0.65;

    LayerNode deepestPass;
    deepestPass.type = LayerType::Group;
    deepestPass.name = QStringLiteral("Deep Pass");
    deepestPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    deepestPass.children = {deepestExposure};
    deepestPass.opacity = 0.72;
    deepestPass.maskImage = innerMask;
    deepestPass.maskInverted = true;

    LayerNode childRaster;
    childRaster.type = LayerType::Raster;
    childRaster.name = QStringLiteral("Nested Raster");
    childRaster.rasterImage = raster;
    childRaster.opacity = 0.48;
    childRaster.blendMode = BlendMode::Screen;
    childRaster.maskImage = outerMask;

    LayerNode isolated;
    isolated.type = LayerType::Group;
    isolated.name = QStringLiteral("Isolation Boundary");
    isolated.groupCompositeMode = GroupCompositeMode::Isolated;
    isolated.children = {deepestPass, childRaster};
    isolated.opacity = 0.83;
    isolated.maskImage = innerMask;
    isolated.transform = QTransform::fromTranslate(3.0, -2.0);

    LayerNode contrast;
    contrast.type = LayerType::Adjustment;
    contrast.adjustmentType = AdjustmentType::Contrast;
    contrast.contrast = 24.0;

    LayerNode middlePass;
    middlePass.type = LayerType::Group;
    middlePass.name = QStringLiteral("Middle Pass");
    middlePass.groupCompositeMode = GroupCompositeMode::PassThrough;
    middlePass.children = {contrast, isolated};
    middlePass.opacity = 0.67;
    middlePass.maskImage = outerMask;
    middlePass.maskEnabled = false;

    LayerNode saturation;
    saturation.type = LayerType::Adjustment;
    saturation.adjustmentType = AdjustmentType::Saturation;
    saturation.saturation = 18.0;

    LayerNode outerPass;
    outerPass.type = LayerType::Group;
    outerPass.name = QStringLiteral("Outer Pass");
    outerPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    outerPass.children = {saturation, middlePass};
    outerPass.opacity = 0.79;
    outerPass.maskImage = outerMask;
    outerPass.transform = QTransform::fromTranslate(-2.0, 1.0);

    const QVector<LayerNode> layers {outerPass, base};
    const QImage full = ImageProcessor::render(source, layers, nullptr, size);
    QVERIFY(!full.isNull());
    const QVector<QRect> regions {
        QRect(0, 0, 256, 256),
        QRect(240, 17, 280, 271),
        QRect(255, 127, 265, 183),
        QRect(77, 201, 311, 109)
    };
    for (const QRect &region : regions) {
        const QRect clipped = region.intersected(source.rect());
        QCOMPARE(ImageProcessor::renderRegion(source, layers, clipped, size),
                 full.copy(clipped));
    }

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("deep-nested.png"));
    base.id = document.baseLayerId();
    QVERIFY(document.replaceLayerTree({outerPass, base}));
    const QImage documentRender = ImageProcessor::render(document.sourceImage(),
                                                         document.layers(),
                                                         nullptr,
                                                         size);
    QCOMPARE(documentRender, full);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("deep-nested.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(loaded.loadWarnings().isEmpty());
    QCOMPARE(ImageProcessor::render(loaded.sourceImage(),
                                    loaded.layers(),
                                    nullptr,
                                    size),
             full);
    const LayerNode loadedOuter = loaded.layerById(outerPass.id);
    const LayerNode loadedMiddle = loaded.layerById(middlePass.id);
    const LayerNode loadedIsolated = loaded.layerById(isolated.id);
    const LayerNode loadedDeep = loaded.layerById(deepestPass.id);
    QCOMPARE(static_cast<int>(loadedOuter.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QCOMPARE(static_cast<int>(loadedMiddle.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QCOMPARE(static_cast<int>(loadedIsolated.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::Isolated));
    QCOMPARE(static_cast<int>(loadedDeep.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QVERIFY(!loadedMiddle.maskEnabled);
    QVERIFY(loadedDeep.maskInverted);
    QCOMPARE(loadedOuter.maskImage.size(), size);
    QCOMPARE(loadedDeep.maskImage.size(), size);

    QHash<QUuid, QUuid> idMap;
    const QVector<QUuid> duplicates = loaded.duplicateLayers({outerPass.id}, &idMap);
    QCOMPARE(duplicates.size(), 1);
    const QUuid destination = loaded.addGroup(loaded.baseLayerId());
    QVERIFY(!destination.isNull());
    QVERIFY(loaded.updateLayer(destination, [](LayerNode &layer) {
        layer.groupCompositeMode = GroupCompositeMode::PassThrough;
    }));
    QVERIFY(loaded.moveLayers(duplicates, destination, 0));
    const LayerNode movedOuter = loaded.layerById(idMap.value(outerPass.id));
    const LayerNode movedMiddle = loaded.layerById(idMap.value(middlePass.id));
    const LayerNode movedIsolated = loaded.layerById(idMap.value(isolated.id));
    const LayerNode movedDeep = loaded.layerById(idMap.value(deepestPass.id));
    QCOMPARE(static_cast<int>(movedOuter.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QCOMPARE(static_cast<int>(movedMiddle.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QCOMPARE(static_cast<int>(movedIsolated.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::Isolated));
    QCOMPARE(static_cast<int>(movedDeep.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QVERIFY(movedDeep.maskInverted);
    QVERIFY(!movedMiddle.maskEnabled);
}

void CoreTests::cancelledTiledRenderDoesNotPublishObsoleteTiles()
{
    QImage source(520, 310, QImage::Format_RGBA8888);
    source.fill(QColor(54, 72, 91, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 0.7;
    LayerNode group;
    group.type = LayerType::Group;
    group.groupCompositeMode = GroupCompositeMode::PassThrough;
    group.children = {exposure};
    QVector<LayerNode> layers {group, base};

    TiledCanvasEngine engine;
    std::atomic_bool cancelled {true};
    TiledCanvasEngine::RenderInfo cancelledInfo;
    QVERIFY(engine.renderRegion(source,
                                layers,
                                source.rect(),
                                source.size(),
                                false,
                                0,
                                &cancelled,
                                &cancelledInfo).isNull());
    QVERIFY(cancelledInfo.cancelled);
    QCOMPARE(engine.cacheStats().residentTiles, 0);
    QCOMPARE(engine.cancelledCompositeTiles(), quint64(1));

    cancelled.store(false, std::memory_order_release);
    TiledCanvasEngine::RenderInfo firstInfo;
    const QImage first = engine.renderRegion(source,
                                             layers,
                                             source.rect(),
                                             source.size(),
                                             false,
                                             0,
                                             &cancelled,
                                             &firstInfo);
    QVERIFY(!first.isNull());
    QVERIFY(firstInfo.path.contains(QStringLiteral("Pass Through")));
    QVERIFY(engine.cacheStats().residentTiles > 0);

    layers[0].children[0].exposure = 1.4;
    cancelled.store(true, std::memory_order_release);
    TiledCanvasEngine::RenderInfo staleInfo;
    QVERIFY(engine.renderRegion(source,
                                layers,
                                source.rect(),
                                source.size(),
                                false,
                                0,
                                &cancelled,
                                &staleInfo).isNull());
    QVERIFY(staleInfo.cancelled);

    cancelled.store(false, std::memory_order_release);
    const QImage second = engine.renderRegion(source,
                                              layers,
                                              source.rect(),
                                              source.size(),
                                              false,
                                              0,
                                              &cancelled);
    QVERIFY(!second.isNull());
    QVERIFY(second.pixelColor(10, 10) != first.pixelColor(10, 10));
}

void CoreTests::renderInfoDescribesVisibleNestedHierarchy()
{
    QImage source(48, 32, QImage::Format_RGBA8888);
    source.fill(QColor(80, 90, 100, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 0.5;
    LayerNode deepPass;
    deepPass.type = LayerType::Group;
    deepPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    deepPass.children = {exposure};
    LayerNode isolated;
    isolated.type = LayerType::Group;
    isolated.groupCompositeMode = GroupCompositeMode::Isolated;
    isolated.children = {deepPass};
    LayerNode outerPass;
    outerPass.type = LayerType::Group;
    outerPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    outerPass.children = {isolated};
    LayerNode hiddenPass = deepPass;
    hiddenPass.id = QUuid::createUuid();
    hiddenPass.visible = false;

    TiledCanvasEngine engine;
    TiledCanvasEngine::RenderInfo info;
    const QImage rendered = engine.renderRegion(source,
                                                {hiddenPass, outerPass, base},
                                                source.rect(),
                                                source.size(),
                                                false,
                                                0,
                                                nullptr,
                                                &info);
    QVERIFY(!rendered.isNull());
    QCOMPARE(info.visiblePassThroughGroups, 2);
    QCOMPARE(info.visibleIsolatedGroups, 1);
    QCOMPARE(info.maximumGroupDepth, 3);
    QVERIFY(!info.usedGpu);
    QVERIFY(!info.mixedBackend);
    QVERIFY(info.path.contains(QStringLiteral("CPU tiled reference")));
    QVERIFY(info.path.contains(QStringLiteral("Pass Through")));

    RenderBackend &backend = RenderBackend::instance();
    backend.setDisplayedRenderInfo(info, 42, 0);
    const QString status = backend.statusText();
    QVERIFY(status.contains(QStringLiteral("Current displayed document: CPU tiled reference")));
    QVERIFY(status.contains(QStringLiteral("2 Pass Through, 1 Isolated")));
    QVERIFY(status.contains(QStringLiteral("maximum nesting depth 3")));
    QVERIFY(status.contains(QStringLiteral("published generation 42 at level 0")));

    TiledCanvasEngine::RenderInfo obsolete = info;
    obsolete.path = QStringLiteral("Obsolete path must not become displayed");
    obsolete.fallbackReason = QStringLiteral("Obsolete fallback marker");
    backend.setDisplayedRenderInfo(obsolete, 41, 0);
    backend.setDisplayedRenderInfo(obsolete, 42, 2);
    const QString unchangedStatus = backend.statusText();
    QVERIFY(!unchangedStatus.contains(QStringLiteral("Obsolete fallback marker")));
    QVERIFY(unchangedStatus.contains(QStringLiteral("published generation 42 at level 0")));
}

void CoreTests::passThroughGroupsUseTiledReferenceWithoutGpu()
{
    QImage source(32, 24, QImage::Format_RGBA8888);
    source.fill(QColor(55, 70, 85, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 0.75;

    LayerNode group;
    group.type = LayerType::Group;
    group.groupCompositeMode = GroupCompositeMode::PassThrough;
    group.children = {exposure};

    const QVector<LayerNode> layers {group, base};
    TiledCanvasEngine engine;
    const QImage tiled = engine.renderRegion(source,
                                             layers,
                                             source.rect(),
                                             source.size(),
                                             true,
                                             0);
    QCOMPARE(tiled, ImageProcessor::render(source, layers));
    QCOMPARE(engine.lastBackendText(),
             QStringLiteral("CPU tiled reference compositor + Pass Through groups"));

    group.visible = false;
    const QVector<LayerNode> hiddenLayers {group, base};
    QCOMPARE(engine.renderRegion(source,
                                 hiddenLayers,
                                 source.rect(),
                                 source.size(),
                                 true,
                                 0),
             source);
    QCOMPARE(engine.lastBackendText(), QStringLiteral("CPU tiled reference compositor"));
}

void CoreTests::missingGroupCompositeModeDefaultsToIsolated()
{
    LayerNode group;
    group.type = LayerType::Group;
    group.name = QStringLiteral("Legacy Group");
    QJsonObject object = group.toJson();
    object.remove(QStringLiteral("compositingMode"));

    bool ok = false;
    QStringList warnings;
    const LayerNode loaded = LayerNode::fromJson(object, &ok, &warnings);
    QVERIFY(ok);
    QVERIFY(warnings.isEmpty());
    QCOMPARE(static_cast<int>(loaded.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::Isolated));
}

void CoreTests::movingLayersIntoGroupUpdatesModelImmediately()
{
    QImage source(8, 8, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("move.png"));

    const QUuid first = document.addRasterLayer(document.baseLayerId());
    const QUuid second = document.addRasterLayer(first);
    const QUuid group = document.addGroup(document.baseLayerId());
    QVERIFY(!first.isNull());
    QVERIFY(!second.isNull());
    QVERIFY(!group.isNull());

    QVERIFY(document.moveLayers({first, second}, group, 0));
    const LayerNode groupNode = document.layerById(group);
    QCOMPARE(groupNode.children.size(), 2);
    QCOMPARE(groupNode.children.at(0).id, second);
    QCOMPARE(groupNode.children.at(1).id, first);
}

void CoreTests::movingBetweenGroupsPreservesWorldPosition()
{
    QImage source(16, 16, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("transforms.png"));

    const QUuid sourceGroup = document.addGroup(document.baseLayerId());
    const QUuid raster = document.addRasterLayer(sourceGroup);
    const QUuid destinationGroup = document.addGroup(document.baseLayerId());
    QVERIFY(document.updateLayer(sourceGroup, [](LayerNode &layer) {
        QTransform transform;
        transform.scale(1.2, 0.85);
        transform.rotate(13.0);
        transform.translate(20.0, 10.0);
        layer.transform = transform;
    }));
    QVERIFY(document.updateLayer(raster, [](LayerNode &layer) {
        QTransform transform;
        transform.rotate(-8.0);
        transform.translate(4.0, 3.0);
        layer.transform = transform;
        layer.rasterImage = QImage(16, 16, QImage::Format_ARGB32_Premultiplied);
        layer.rasterImage.fill(Qt::transparent);
        layer.rasterImage.setPixelColor(6, 5, QColor(220, 60, 40, 230));
    }));
    QVERIFY(document.addMask(raster));
    QVERIFY(document.updateLayer(raster, [](LayerNode &layer) {
        layer.maskImage = QImage(16, 16, QImage::Format_Grayscale8);
        layer.maskImage.fill(255);
        layer.maskImage.scanLine(5)[6] = 140;
        layer.maskInverted = true;
    }));
    QVERIFY(document.updateLayer(destinationGroup, [](LayerNode &layer) {
        QTransform transform;
        transform.scale(0.9, 1.1);
        transform.rotate(21.0);
        transform.translate(-7.0, 5.0);
        layer.transform = transform;
    }));

    const QTransform before = document.layerWorldTransform(raster);
    const QImage beforeRender = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, source.size());
    QVERIFY(document.moveLayers({raster}, destinationGroup, 0));
    QVERIFY(transformsClose(document.layerWorldTransform(raster), before));
    const LayerNode moved = document.layerById(raster);
    QVERIFY(moved.hasMask());
    QVERIFY(moved.maskInverted);
    QCOMPARE(moved.maskImage.constScanLine(5)[6], uchar(140));
    const QImage afterRender = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, source.size());
    QCOMPARE(afterRender, beforeRender);
}

void CoreTests::groupingSelectionCreatesGroupWithChildren()
{
    QImage source(8, 8, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("group.png"));

    const QUuid first = document.addRasterLayer(document.baseLayerId());
    const QUuid second = document.addAdjustment(AdjustmentType::Contrast, first);
    QVERIFY(!first.isNull());
    QVERIFY(!second.isNull());

    const QUuid group = document.groupLayers({first, second}, QStringLiteral("Selection"));
    QVERIFY(!group.isNull());
    const LayerNode groupNode = document.layerById(group);
    QCOMPARE(groupNode.name, QStringLiteral("Selection"));
    QCOMPARE(groupNode.children.size(), 2);
    QCOMPARE(groupNode.children.at(0).id, second);
    QCOMPARE(groupNode.children.at(1).id, first);
    QVERIFY(document.containsLayer(first));
    QVERIFY(document.containsLayer(second));
}

void CoreTests::selectedAncestorMovesOnlyOnce()
{
    QImage source(8, 8, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("roots.png"));

    const QUuid group = document.addGroup(document.baseLayerId());
    const QUuid child = document.addRasterLayer(group);
    const QUuid destination = document.addGroup(document.baseLayerId());
    QVERIFY(document.moveLayers({group, child}, destination, 0));

    const LayerNode destinationNode = document.layerById(destination);
    QCOMPARE(destinationNode.children.size(), 1);
    QCOMPARE(destinationNode.children.constFirst().id, group);
    QCOMPARE(destinationNode.children.constFirst().children.size(), 1);
    QCOMPARE(destinationNode.children.constFirst().children.constFirst().id, child);
}

void CoreTests::layerNodeEqualityTracksRasterRevisions()
{
    LayerNode first;
    first.type = LayerType::Raster;
    first.name = QStringLiteral("Paint");
    first.rasterImage = QImage(4, 4, QImage::Format_RGBA8888);
    first.rasterImage.fill(QColor(20, 40, 60, 255));

    LayerNode child;
    child.type = LayerType::Adjustment;
    child.adjustmentType = AdjustmentType::Exposure;
    child.exposure = 0.25;
    first.children.push_back(child);

    LayerNode second = first;
    QVERIFY(first == second);

    second.opacity = 0.5;
    QVERIFY(!(first == second));

    second = first;
    second.rasterImage.setPixelColor(1, 1, QColor(255, 0, 0, 255));
    QVERIFY(!(first == second));

    second = first;
    second.children[0].exposure = 0.5;
    QVERIFY(!(first == second));
}

void CoreTests::tgaRoundTripPreservesPixels()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(2, 2, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    source.setPixelColor(1, 0, QColor(0, 255, 0, 200));
    source.setPixelColor(0, 1, QColor(0, 0, 255, 128));
    source.setPixelColor(1, 1, QColor(255, 255, 255, 0));

    const QString path = directory.filePath(QStringLiteral("roundtrip.tga"));
    QString error;
    QVERIFY2(TgaCodec::write(path, source, &error), qPrintable(error));
    const QImage loaded = TgaCodec::read(path, &error);
    QVERIFY2(!loaded.isNull(), qPrintable(error));
    QCOMPARE(loaded.size(), source.size());

    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            QCOMPARE(loaded.pixelColor(x, y), source.pixelColor(x, y));
        }
    }
}


void CoreTests::tgaRoundTripPreservesHiddenRgbAtZeroAlpha()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(3, 1, QImage::Format_RGBA8888);
    uchar *row = source.scanLine(0);
    const uchar pixels[12] {
        214, 63, 171, 0,
        12, 250, 44, 0,
        99, 18, 231, 37
    };
    std::copy_n(pixels, 12, row);

    const QString path = directory.filePath(QStringLiteral("hidden-rgb.tga"));
    QString error;
    QVERIFY2(TgaCodec::write(path, source, &error), qPrintable(error));
    const QImage loaded = TgaCodec::read(path, &error).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY2(!loaded.isNull(), qPrintable(error));
    QCOMPARE(loaded.size(), source.size());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(loaded.constScanLine(0)), 12),
             QByteArray(reinterpret_cast<const char *>(source.constScanLine(0)), 12));
}

void CoreTests::alphaSafeFlattenedExportPreservesHiddenRgb()
{
    QImage source(3, 1, QImage::Format_RGBA8888);
    uchar *sourceRow = source.scanLine(0);
    const uchar pixels[12] {
        214, 63, 171, 0,
        173, 91, 220, 1,
        18, 202, 91, 255
    };
    std::copy_n(pixels, 12, sourceRow);

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.name = QStringLiteral("Base Image");

    const QImage rendered = ImageProcessor::renderPreservingHiddenRgb(source,
                                                                       {base},
                                                                       nullptr,
                                                                       source.size())
                                .convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!rendered.isNull());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(rendered.constScanLine(0)), 12),
             QByteArray(reinterpret_cast<const char *>(source.constScanLine(0)), 12));
}

void CoreTests::baseOverrideRendersInsteadOfSource()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.fill(QColor(240, 20, 30, 255));

    QImage overridePixels(2, 1, QImage::Format_RGBA8888);
    overridePixels.fill(QColor(25, 60, 220, 255));
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = overridePixels;

    const QImage rendered = ImageProcessor::render(source, {base})
                                .convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.pixelColor(0, 0), QColor(25, 60, 220, 255));
}

void CoreTests::projectSourceRoundTripPreservesHiddenRgb()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(2, 1, QImage::Format_RGBA8888);
    uchar *row = source.scanLine(0);
    const uchar pixels[8] {214, 63, 171, 0, 16, 201, 91, 255};
    std::copy_n(pixels, 8, row);

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("hidden-source.tga"));
    const QString path = directory.filePath(QStringLiteral("hidden-source.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    const QImage loadedSource = loaded.sourceImage().convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!loadedSource.isNull());
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(loadedSource.constScanLine(0)), 8),
             QByteArray(reinterpret_cast<const char *>(source.constScanLine(0)), 8));
}

void CoreTests::baseOverrideProjectRoundTripPreservesHiddenRgb()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(2, 2, QImage::Format_RGBA8888);
    source.fill(QColor(10, 20, 30, 255));
    QImage overridePixels = source;
    overridePixels.detach();
    uchar *row = overridePixels.scanLine(0);
    row[0] = 201;
    row[1] = 72;
    row[2] = 149;
    row[3] = 0;

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("source.png"));
    QVERIFY(document.updateLayer(document.baseLayerId(), [&overridePixels](LayerNode &layer) {
        layer.rasterImage = overridePixels;
    }));

    const QString path = directory.filePath(QStringLiteral("base-channel-edit.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QCOMPARE(PhotoDocument::ProjectFormatVersion, 27);
    const QImage pixels = loaded.layerById(loaded.baseLayerId()).rasterImage
                              .convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!pixels.isNull());
    const uchar *loadedRow = pixels.constScanLine(0);
    QCOMPARE(loadedRow[0], static_cast<uchar>(201));
    QCOMPARE(loadedRow[1], static_cast<uchar>(72));
    QCOMPARE(loadedRow[2], static_cast<uchar>(149));
    QCOMPARE(loadedRow[3], static_cast<uchar>(0));
}

void CoreTests::authoritativeFlattenedExportRoundTripsCombinedWorkflow()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QSize size(258, 258);
    QImage source(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            source.setPixelColor(x, y, QColor((x * 5 + y * 3) & 255,
                                              (x * 2 + y * 7) & 255,
                                              (x * 11 + y) & 255,
                                              120 + ((x + y) & 127)));
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode painted;
    painted.type = LayerType::Raster;
    painted.opacity = 0.72;
    painted.blendMode = BlendMode::Screen;
    painted.rasterImage = QImage(size, QImage::Format_RGBA8888);
    painted.rasterImage.fill(Qt::transparent);
    QPainter rasterPainter(&painted.rasterImage);
    rasterPainter.setPen(Qt::NoPen);
    rasterPainter.setBrush(QColor(210, 35, 160, 190));
    rasterPainter.drawEllipse(QRect(94, 78, 146, 133));
    rasterPainter.end();
    painted.maskImage = QImage(size, QImage::Format_Grayscale8);
    painted.maskImage.fill(255);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = painted.maskImage.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            if (x >= 252 || y >= 252) {
                row[x] = static_cast<uchar>((x * 13 + y * 17) & 255);
            }
        }
    }

    LayerNode levels;
    levels.type = LayerType::Adjustment;
    levels.adjustmentType = AdjustmentType::Levels;
    levels.blackPoint = 0.08;
    levels.whitePoint = 0.91;
    levels.gamma = 1.18;
    levels.maskImage = QImage(size, QImage::Format_Grayscale8);
    levels.maskImage.fill(255);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = levels.maskImage.scanLine(y);
        std::fill(row, row + 129, uchar(96));
    }

    LayerNode isolated;
    isolated.type = LayerType::Group;
    isolated.groupCompositeMode = GroupCompositeMode::Isolated;
    isolated.opacity = 0.63;
    isolated.children = {painted};

    LayerNode passThrough;
    passThrough.type = LayerType::Group;
    passThrough.groupCompositeMode = GroupCompositeMode::PassThrough;
    passThrough.opacity = 0.84;
    passThrough.children = {levels, isolated};
    passThrough.maskImage = QImage(size, QImage::Format_Grayscale8);
    passThrough.maskImage.fill(255);
    passThrough.maskImage.scanLine(255)[255] = 0;

    const QVector<LayerNode> layers {passThrough, base};
    const QImage expected = ImageProcessor::render(source, layers, nullptr, size);
    QVERIFY(!expected.isNull());

    const QString path = directory.filePath(QStringLiteral("combined-workflow.png"));
    QString error;
    QVERIFY2(PhotoDocument::writeImageFile(path, expected, 95, &error), qPrintable(error));
    const QImage loaded = PhotoDocument::readImageFile(path, &error);
    QVERIFY2(!loaded.isNull(), qPrintable(error));
    const QImage loadedRgba = loaded.convertToFormat(QImage::Format_RGBA8888);
    const QImage expectedRgba = expected.convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(loadedRgba.size(), expectedRgba.size());
    for (int y = 0; y < expectedRgba.height(); ++y) {
        for (int x = 0; x < expectedRgba.width(); ++x) {
            QCOMPARE(loadedRgba.pixelColor(x, y), expectedRgba.pixelColor(x, y));
        }
    }
}

void CoreTests::failedImageExportPreservesExistingFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("protected.unsupported-format"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    const QByteArray marker("existing-output-must-survive");
    QCOMPARE(existing.write(marker), qint64(marker.size()));
    existing.close();

    QImage image(8, 8, QImage::Format_RGBA8888);
    image.fill(QColor(20, 40, 80, 255));
    QString error;
    QVERIFY(!PhotoDocument::writeImageFile(path, image, 95, &error));
    QVERIFY(!error.isEmpty());

    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), marker);
}

void CoreTests::projectLoadNormalisesUnexpectedRasterSize()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QSize documentSize(12, 10);
    QImage source(documentSize, QImage::Format_RGBA8888);
    source.fill(QColor(25, 50, 75, 255));

    PhotoDocument document;
    document.setSourceImage(source);
    const QUuid rasterId = document.addRasterLayer(document.baseLayerId());
    QVERIFY(!rasterId.isNull());
    QVERIFY(document.updateLayer(rasterId, [documentSize](LayerNode &layer) {
        layer.name = QStringLiteral("Repair Me");
        layer.rasterImage = QImage(documentSize, QImage::Format_RGBA8888);
        layer.rasterImage.fill(QColor(200, 30, 90, 180));
    }));

    const QString path = directory.filePath(QStringLiteral("raster-repair.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = json.object();
    QJsonArray layers = root.value(QStringLiteral("layerTree")).toArray();
    QImage wrongSize(3, 2, QImage::Format_RGBA8888);
    wrongSize.fill(QColor(220, 10, 30, 200));
    const QString encoded = encodePngBase64(wrongSize);
    QVERIFY(!encoded.isEmpty());
    QVERIFY(mutateLayerObject(&layers, rasterId, [encoded](QJsonObject &layer) {
        layer.insert(QStringLiteral("rasterEncoding"), QStringLiteral("png-base64"));
        layer.insert(QStringLiteral("rasterData"), encoded);
    }));
    root.insert(QStringLiteral("layerTree"), layers);
    json.setObject(root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = json.toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(!loaded.loadWarnings().isEmpty());
    const LayerNode repaired = loaded.layerById(rasterId);
    QCOMPARE(repaired.rasterImage.size(), documentSize);
    const QColor centre = repaired.rasterImage.pixelColor(documentSize.width() / 2,
                                                           documentSize.height() / 2);
    QVERIFY(centre.red() > 180);
    QVERIFY(centre.alpha() > 150);
}

void CoreTests::renderBackendResetClearsDisplayedDocumentInfo()
{
    RenderBackend &backend = RenderBackend::instance();
    TiledCanvasEngine::RenderInfo info;
    info.path = QStringLiteral("Synthetic accepted path");
    info.visiblePassThroughGroups = 2;
    info.visibleIsolatedGroups = 1;
    info.maximumGroupDepth = 3;
    backend.setDisplayedRenderInfo(info, 77, 0);
    QVERIFY(backend.statusText().contains(QStringLiteral("Synthetic accepted path")));

    backend.resetDocumentState();
    const QString resetStatus = backend.statusText();
    QVERIFY(!resetStatus.contains(QStringLiteral("Synthetic accepted path")));
    QVERIFY(resetStatus.contains(QStringLiteral("has not been published yet")));
}

void CoreTests::projectRoundTripPreservesLayerTree()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(8, 6, QImage::Format_RGBA64);
    source.fill(QColor(20, 40, 80, 255));

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("example.png"));
    const QUuid groupId = document.addGroup(document.baseLayerId());
    QVERIFY(!groupId.isNull());
    QTransform expectedGroupTransform;
    expectedGroupTransform.scale(1.25, 0.8);
    expectedGroupTransform.rotate(17.0);
    expectedGroupTransform.translate(12.5, -3.0);
    QVERIFY(document.updateLayer(groupId, [expectedGroupTransform](LayerNode &group) {
        group.name = QStringLiteral("Retouch Group");
        group.opacity = 0.75;
        group.blendMode = BlendMode::Overlay;
        group.transform = expectedGroupTransform;
    }));
    QVERIFY(document.addMask(groupId));
    QVERIFY(document.setMaskEnabled(groupId, false));
    QVERIFY(document.setMaskInverted(groupId, true));

    const QUuid adjustmentId = document.addAdjustment(AdjustmentType::Levels, groupId);
    QVERIFY(document.updateLayer(adjustmentId, [](LayerNode &layer) {
        layer.name = QStringLiteral("Test Levels");
        layer.blackPoint = 0.1;
        layer.whitePoint = 0.9;
        layer.gamma = 1.2;
    }));

    const QUuid rasterId = document.addRasterLayer(groupId);
    QVERIFY(document.updateLayer(rasterId, [](LayerNode &layer) {
        layer.name = QStringLiteral("Paint");
        layer.opacity = 0.5;
        layer.blendMode = BlendMode::Multiply;
        layer.rasterImage = QImage(8, 6, QImage::Format_ARGB32_Premultiplied);
        layer.rasterImage.fill(Qt::transparent);
        layer.rasterImage.setPixelColor(3, 2, QColor(240, 20, 90, 180));
    }));
    document.setGuides({1.5, 4.0}, {2.0, 6.0});

    const QImage expectedFlattened = ImageProcessor::render(
        document.sourceImage(), document.layers(), nullptr, source.size());

    const QString path = directory.filePath(QStringLiteral("test.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(loaded.loadWarnings().isEmpty());
    QCOMPARE(loaded.sourceImage().size(), source.size());
    QCOMPARE(loaded.layerCount(), 4);
    QVERIFY(loaded.containsLayer(groupId));
    QVERIFY(loaded.containsLayer(adjustmentId));
    QVERIFY(loaded.containsLayer(rasterId));

    const LayerNode group = loaded.layerById(groupId);
    QCOMPARE(group.name, QStringLiteral("Retouch Group"));
    QCOMPARE(group.opacity, 0.75);
    QCOMPARE(static_cast<int>(group.blendMode), static_cast<int>(BlendMode::Overlay));
    QVERIFY(transformsClose(group.transform, expectedGroupTransform));
    QVERIFY(group.hasMask());
    QVERIFY(!group.maskEnabled);
    QVERIFY(group.maskInverted);
    QCOMPARE(group.children.size(), 2);

    const LayerNode adjustment = loaded.layerById(adjustmentId);
    QCOMPARE(adjustment.name, QStringLiteral("Test Levels"));
    QCOMPARE(adjustment.blackPoint, 0.1);
    QCOMPARE(adjustment.whitePoint, 0.9);
    QCOMPARE(adjustment.gamma, 1.2);

    QCOMPARE(loaded.horizontalGuides().size(), 2);
    QCOMPARE(loaded.horizontalGuides().at(0), 1.5);
    QCOMPARE(loaded.horizontalGuides().at(1), 4.0);
    QCOMPARE(loaded.verticalGuides().size(), 2);
    QCOMPARE(loaded.verticalGuides().at(0), 2.0);
    QCOMPARE(loaded.verticalGuides().at(1), 6.0);

    const LayerNode raster = loaded.layerById(rasterId);
    QVERIFY(!raster.rasterImage.isNull());
    QCOMPARE(raster.rasterImage.size(), QSize(8, 6));
    QCOMPARE(raster.rasterImage.pixelColor(3, 2), QColor(240, 20, 90, 180));
    const QImage loadedFlattened = ImageProcessor::render(
        loaded.sourceImage(), loaded.layers(), nullptr, source.size());
    QCOMPARE(loadedFlattened, expectedFlattened);
    QVERIFY(!loaded.isModified());
}

void CoreTests::projectRoundTripPreservesProjectiveGroupTransform()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(64, 48, QImage::Format_RGBA8888);
    source.fill(QColor(30, 50, 80, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("projective-group.png"));
    const QUuid groupId = document.addGroup(document.baseLayerId());
    QVERIFY(!groupId.isNull());

    QPolygonF from;
    from << QPointF(8.0, 6.0) << QPointF(56.0, 6.0)
         << QPointF(56.0, 42.0) << QPointF(8.0, 42.0);
    QPolygonF to;
    to << QPointF(12.0, 9.0) << QPointF(52.0, 4.0)
       << QPointF(60.0, 44.0) << QPointF(5.0, 39.0);
    QTransform projective;
    QVERIFY(QTransform::quadToQuad(from, to, projective));
    QCOMPARE(projective.type(), QTransform::TxProject);
    QVERIFY(document.updateLayer(groupId, [projective](LayerNode &group) {
        group.name = QStringLiteral("Projective Group");
        group.transform = projective;
    }));

    const QString path = directory.filePath(QStringLiteral("projective.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(loaded.loadWarnings().isEmpty());
    const LayerNode restored = loaded.layerById(groupId);
    QCOMPARE(restored.name, QStringLiteral("Projective Group"));
    QVERIFY(transformsClose(restored.transform, projective));
    QCOMPARE(restored.transform.type(), QTransform::TxProject);
}

void CoreTests::projectRoundTripPreservesPassThroughMode()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(6, 4, QImage::Format_RGBA8888);
    source.fill(QColor(50, 70, 90, 255));

    PhotoDocument document;
    document.setSourceImage(source);
    const QUuid groupId = document.addGroup(document.baseLayerId());
    QVERIFY(!groupId.isNull());
    QVERIFY(document.updateLayer(groupId, [](LayerNode &group) {
        group.name = QStringLiteral("Pass Through Group");
        group.groupCompositeMode = GroupCompositeMode::PassThrough;
        group.opacity = 0.65;
    }));
    const QUuid adjustmentId = document.addAdjustment(AdjustmentType::Contrast, groupId);
    QVERIFY(!adjustmentId.isNull());
    QVERIFY(document.updateLayer(adjustmentId, [](LayerNode &adjustment) {
        adjustment.contrast = 45.0;
    }));

    const QImage expected = ImageProcessor::render(source,
                                                    document.layers(),
                                                    nullptr,
                                                    source.size());
    const QString path = directory.filePath(QStringLiteral("pass-through.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(loaded.loadWarnings().isEmpty());
    const LayerNode group = loaded.layerById(groupId);
    QCOMPARE(static_cast<int>(group.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QCOMPARE(group.opacity, 0.65);
    QCOMPARE(ImageProcessor::render(loaded.sourceImage(),
                                    loaded.layers(),
                                    nullptr,
                                    source.size()),
             expected);
}

void CoreTests::projectLoadRepairsInvalidGroupCompositeMode()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument document;
    QImage source(5, 5, QImage::Format_RGBA8888);
    source.fill(QColor(40, 60, 80, 255));
    document.setSourceImage(source);
    const QUuid groupId = document.addGroup(document.baseLayerId());
    QVERIFY(!groupId.isNull());

    const QString path = directory.filePath(QStringLiteral("invalid-group-mode.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject root = json.object();
    QJsonArray layers = root.value(QStringLiteral("layerTree")).toArray();
    QVERIFY(mutateLayerObject(&layers, groupId, [](QJsonObject &group) {
        group.insert(QStringLiteral("compositingMode"), QStringLiteral("unknown-mode"));
    }));
    root.insert(QStringLiteral("layerTree"), layers);
    json.setObject(root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray bytes = json.toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(!loaded.loadWarnings().isEmpty());
    QCOMPARE(static_cast<int>(loaded.layerById(groupId).groupCompositeMode),
             static_cast<int>(GroupCompositeMode::Isolated));
    QVERIFY(!loaded.isModified());
}

void CoreTests::duplicateLayersPreserveMasksAndGenerateFreshIds()
{
    PhotoDocument document;
    QImage source(16, 12, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(30, 50, 70, 255));
    document.setSourceImage(source);

    const QUuid rasterId = document.addRasterLayer(document.baseLayerId());
    QVERIFY(!rasterId.isNull());
    QVERIFY(document.updateLayer(rasterId, [](LayerNode &layer) {
        layer.name = QStringLiteral("Paint");
        layer.opacity = 0.65;
        layer.blendMode = BlendMode::Overlay;
        layer.transform = QTransform::fromTranslate(3.0, -2.0);
        layer.rasterImage = QImage(16, 12, QImage::Format_ARGB32_Premultiplied);
        layer.rasterImage.fill(Qt::transparent);
        layer.rasterImage.setPixelColor(5, 4, QColor(220, 40, 90, 180));
    }));
    QVERIFY(document.addMask(rasterId));
    QVERIFY(document.updateLayer(rasterId, [](LayerNode &layer) {
        layer.maskImage = QImage(16, 12, QImage::Format_Grayscale8);
        layer.maskImage.fill(255);
        layer.maskImage.scanLine(4)[5] = 32;
        layer.maskEnabled = false;
        layer.maskInverted = true;
    }));

    const QUuid adjustmentId = document.addAdjustment(AdjustmentType::Levels, rasterId);
    QVERIFY(document.updateLayer(adjustmentId, [](LayerNode &layer) {
        layer.name = QStringLiteral("Levels A");
        layer.blackPoint = 0.12;
        layer.whitePoint = 0.87;
        layer.gamma = 1.3;
    }));

    QHash<QUuid, QUuid> idMap;
    const QVector<QUuid> duplicateIds = document.duplicateLayers(
        {rasterId, adjustmentId}, &idMap);
    QCOMPARE(duplicateIds.size(), 2);
    QVERIFY(idMap.contains(rasterId));
    QVERIFY(idMap.contains(adjustmentId));
    QVERIFY(idMap.value(rasterId) != rasterId);
    QVERIFY(idMap.value(adjustmentId) != adjustmentId);

    const LayerNode originalRaster = document.layerById(rasterId);
    const LayerNode duplicateRaster = document.layerById(idMap.value(rasterId));
    QCOMPARE(duplicateRaster.opacity, originalRaster.opacity);
    QCOMPARE(static_cast<int>(duplicateRaster.blendMode),
             static_cast<int>(originalRaster.blendMode));
    QVERIFY(transformsClose(duplicateRaster.transform, originalRaster.transform));
    QCOMPARE(duplicateRaster.rasterImage.pixelColor(5, 4),
             originalRaster.rasterImage.pixelColor(5, 4));
    QCOMPARE(duplicateRaster.maskImage.pixelColor(5, 4),
             originalRaster.maskImage.pixelColor(5, 4));
    QCOMPARE(duplicateRaster.maskEnabled, originalRaster.maskEnabled);
    QCOMPARE(duplicateRaster.maskInverted, originalRaster.maskInverted);
    QVERIFY(duplicateRaster.name.startsWith(QStringLiteral("Paint copy")));

    QUuid originalParent;
    QUuid duplicateParent;
    int originalIndex = -1;
    int duplicateIndex = -1;
    QVERIFY(document.layerPlacement(rasterId, &originalParent, &originalIndex));
    QVERIFY(document.layerPlacement(idMap.value(rasterId), &duplicateParent, &duplicateIndex));
    QCOMPARE(duplicateParent, originalParent);
    QCOMPARE(duplicateIndex + 1, originalIndex);

    // QImage payloads may share storage after duplication, but editing the
    // duplicate must detach and leave the source mask unchanged.
    QVERIFY(document.updateLayer(idMap.value(rasterId), [](LayerNode &layer) {
        layer.maskImage.scanLine(4)[5] = 220;
    }));
    QCOMPARE(document.layerById(rasterId).maskImage.constScanLine(4)[5], uchar(32));
    QCOMPARE(document.layerById(idMap.value(rasterId)).maskImage.constScanLine(4)[5], uchar(220));
}

void CoreTests::duplicateNestedGroupRecursivelyRemapsIds()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument document;
    QImage source(10, 8, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(12, 18, 24, 255));
    document.setSourceImage(source);

    const QUuid groupId = document.addGroup(document.baseLayerId());
    const QUuid nestedId = document.addGroup(groupId);
    const QUuid rasterId = document.addRasterLayer(nestedId);
    const QUuid adjustmentId = document.addAdjustment(AdjustmentType::Exposure, groupId);
    QVERIFY(document.updateLayer(groupId, [](LayerNode &group) {
        group.groupCompositeMode = GroupCompositeMode::PassThrough;
    }));
    QVERIFY(document.addMask(groupId));
    QVERIFY(document.addMask(nestedId));
    QVERIFY(document.addMask(rasterId));
    QVERIFY(document.setMaskInverted(nestedId, true));
    QVERIFY(document.updateLayer(rasterId, [](LayerNode &layer) {
        layer.rasterImage = QImage(10, 8, QImage::Format_ARGB32_Premultiplied);
        layer.rasterImage.fill(QColor(80, 120, 200, 160));
        layer.maskImage = QImage(10, 8, QImage::Format_Grayscale8);
        layer.maskImage.fill(190);
    }));

    QHash<QUuid, QUuid> idMap;
    const QVector<QUuid> duplicates = document.duplicateLayers({groupId, rasterId}, &idMap);
    QCOMPARE(duplicates.size(), 1);
    QCOMPARE(idMap.size(), 4);
    QVERIFY(idMap.contains(groupId));
    QVERIFY(idMap.contains(nestedId));
    QVERIFY(idMap.contains(rasterId));
    QVERIFY(idMap.contains(adjustmentId));

    const LayerNode duplicateGroup = document.layerById(idMap.value(groupId));
    QVERIFY(duplicateGroup.hasMask());
    QCOMPARE(static_cast<int>(duplicateGroup.groupCompositeMode),
             static_cast<int>(GroupCompositeMode::PassThrough));
    QCOMPARE(duplicateGroup.children.size(), 2);
    QVERIFY(document.containsLayer(idMap.value(nestedId)));
    QVERIFY(document.containsLayer(idMap.value(rasterId)));
    QVERIFY(document.containsLayer(idMap.value(adjustmentId)));
    QVERIFY(document.layerById(idMap.value(nestedId)).maskInverted);
    QCOMPARE(document.layerById(idMap.value(rasterId)).maskImage.size(), QSize(10, 8));

    QSet<QUuid> allIds;
    for (const LayerNode &layer : document.layers()) {
        collectLayerIds(layer, &allIds);
    }
    QCOMPARE(allIds.size(), document.layerCount());

    const QString path = directory.filePath(QStringLiteral("duplicated.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QCOMPARE(loaded.layerCount(), document.layerCount());
    QVERIFY(loaded.containsLayer(idMap.value(groupId)));
    QVERIFY(loaded.containsLayer(idMap.value(rasterId)));
    QCOMPARE(loaded.layerById(idMap.value(rasterId)).maskImage.size(), QSize(10, 8));
}

void CoreTests::insertLayerRejectsConflictingDescendantIds()
{
    PhotoDocument document;
    QImage source(8, 8, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::black);
    document.setSourceImage(source);

    const QUuid rasterId = document.addRasterLayer();
    QVERIFY(!rasterId.isNull());

    LayerNode group;
    group.type = LayerType::Group;
    LayerNode child;
    child.type = LayerType::Raster;
    child.id = rasterId;
    group.children.push_back(child);
    QVERIFY(!document.insertLayerAt(group, {}, 0));

    LayerNode internallyInvalid;
    internallyInvalid.type = LayerType::Group;
    LayerNode firstChild;
    firstChild.type = LayerType::Raster;
    LayerNode secondChild = firstChild;
    secondChild.id = firstChild.id;
    internallyInvalid.children = {firstChild, secondChild};
    QVERIFY(!document.insertLayerAt(internallyInvalid, {}, 0));

    // The initial image is now an ordinary raster layer. It and a group
    // containing it may both be duplicated like any other layer.
    QCOMPARE(document.duplicateLayers({document.baseLayerId()}).size(), 1);
    const QUuid baseGroup = document.groupLayers(
        {document.baseLayerId()}, QStringLiteral("Base Container"));
    QVERIFY(!baseGroup.isNull());
    QCOMPARE(document.duplicateLayers({baseGroup}).size(), 1);
}

void CoreTests::projectLoadRepairsDamagedMaskWithoutLosingLayer()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument document;
    QImage source(8, 6, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(30, 60, 90, 255));
    document.setSourceImage(source);
    const QUuid rasterId = document.addRasterLayer();
    QVERIFY(document.addMask(rasterId));

    const QString path = directory.filePath(QStringLiteral("damaged-mask.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(json.isObject());
    QJsonObject root = json.object();
    QJsonArray layers = root.value(QStringLiteral("layerTree")).toArray();
    QVERIFY(mutateLayerObject(&layers, rasterId, [](QJsonObject &layer) {
        layer.insert(QStringLiteral("maskData"), QStringLiteral("not-a-valid-png"));
        layer.insert(QStringLiteral("maskEnabled"), QStringLiteral("wrong-type"));
        layer.insert(QStringLiteral("maskInverted"), 42);
    }));
    root.insert(QStringLiteral("layerTree"), layers);
    json.setObject(root);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray repairedBytes = json.toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(repairedBytes), qint64(repairedBytes.size()));
    file.close();

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    QVERIFY(loaded.containsLayer(rasterId));
    QVERIFY(!loaded.layerById(rasterId).hasMask());
    QVERIFY(!loaded.loadWarnings().isEmpty());
}

void CoreTests::projectLoadNormalisesUnexpectedMaskShapeAndFormat()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument document;
    QImage source(8, 6, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(25, 50, 75, 255));
    document.setSourceImage(source);
    const QUuid rasterId = document.addRasterLayer();
    QVERIFY(document.addMask(rasterId));
    QVERIFY(document.updateLayer(rasterId, [](LayerNode &layer) {
        layer.maskImage = QImage(2, 3, QImage::Format_ARGB32_Premultiplied);
        layer.maskImage.fill(QColor(80, 80, 80, 255));
    }));

    const QString path = directory.filePath(QStringLiteral("odd-mask.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    PhotoDocument loaded;
    QVERIFY2(loaded.loadProject(path, &error), qPrintable(error));
    const LayerNode layer = loaded.layerById(rasterId);
    QVERIFY(layer.hasMask());
    QCOMPARE(layer.maskImage.size(), source.size());
    QCOMPARE(layer.maskImage.format(), QImage::Format_Grayscale8);
    QVERIFY(loaded.loadWarnings().size() >= 2);
}

void CoreTests::tileCacheConstructorsUseExpectedBudgets()
{
    TileCache defaultCache;
    const TileCache::Budgets defaultBudgets = defaultCache.budgets();
    QCOMPARE(defaultBudgets.ramBytes, qsizetype(256) * 1024 * 1024);
    QCOMPARE(defaultBudgets.vramBytes, qsizetype(512) * 1024 * 1024);

    TileCache::Budgets requested;
    requested.ramBytes = qsizetype(32) * 1024 * 1024;
    requested.vramBytes = qsizetype(64) * 1024 * 1024;
    TileCache customCache(requested);
    const TileCache::Budgets customBudgets = customCache.budgets();
    QCOMPARE(customBudgets.ramBytes, requested.ramBytes);
    QCOMPARE(customBudgets.vramBytes, requested.vramBytes);
}


void CoreTests::tileCacheSeparatesResolutionLevels()
{
    TileCache cache;
    const QUuid surface = QUuid::createUuid();
    const TileAddress fine {surface, 1, 2, 0, TileDomain::Composite};
    const TileAddress coarse {surface, 1, 2, 2, TileDomain::Composite};

    QImage fineImage(8, 8, QImage::Format_RGBA8888);
    fineImage.fill(QColor(255, 0, 0, 255));
    QImage coarseImage(8, 8, QImage::Format_RGBA8888);
    coarseImage.fill(QColor(0, 0, 255, 255));

    cache.beginUpdate(fine, 11);
    QVERIFY(cache.publish(fine, 11, fineImage, false));
    QVERIFY(cache.markSynchronized(fine, 11));
    cache.beginUpdate(coarse, 22);
    QVERIFY(cache.publish(coarse, 22, coarseImage, false));
    QVERIFY(cache.markSynchronized(coarse, 22));

    const auto fineResult = cache.lookup(fine, 11);
    const auto coarseResult = cache.lookup(coarse, 22);
    QVERIFY(fineResult.has_value());
    QVERIFY(coarseResult.has_value());
    QCOMPARE(fineResult->image.pixelColor(0, 0), QColor(255, 0, 0, 255));
    QCOMPARE(coarseResult->image.pixelColor(0, 0), QColor(0, 0, 255, 255));
}

void CoreTests::progressivePreviewChoosesUsefulCoarseLevels()
{
    const QSize baseSize(2048, 2048);
    const QRect full(QPoint(0, 0), baseSize);
    QCOMPARE(ProgressivePreview::chooseCoarseLevel(1.0, full, baseSize), 2);
    QVERIFY(ProgressivePreview::chooseCoarseLevel(0.25, full, baseSize) >= 2);
    QCOMPARE(ProgressivePreview::chooseCoarseLevel(4.0,
                                                   QRect(800, 800, 256, 256),
                                                   baseSize),
             0);

    QCOMPARE(ProgressivePreview::chooseSpatialInteractionLevel(
                 full, baseSize, 0, true),
             0);
    QCOMPARE(ProgressivePreview::chooseSpatialInteractionLevel(
                 full, baseSize, 0, false),
             2);
    QCOMPARE(ProgressivePreview::chooseSpatialInteractionLevel(
                 QRect(), baseSize, 2, false),
             0);
}

void CoreTests::previewPublicationRejectsSupersededInteractiveRequests()
{
    QVERIFY(previewPublicationIsExact(42, 9, 42, 42, 9));
    QVERIFY(!previewPublicationIsExact(41, 9, 42, 41, 10));
    QVERIFY(!previewPublicationIsExact(42, 9, 42, 42, 10));
    QVERIFY(!previewPublicationIsExact(43, 11, 43, 42, 11));

    QVERIFY(previewPublicationMayAdvanceInteraction(true, true, 41, 9, 41, 41, 9));
    QVERIFY(!previewPublicationMayAdvanceInteraction(false, true, 41, 9, 41, 41, 9));
    QVERIFY(!previewPublicationMayAdvanceInteraction(true, false, 41, 9, 41, 41, 9));
    QVERIFY(!previewPublicationMayAdvanceInteraction(true, true, 41, 8, 41, 41, 9));
    QVERIFY(!previewPublicationMayAdvanceInteraction(true, true, 40, 9, 41, 40, 9));

    QVERIFY(detailPreviewMaySettleWithoutRerender(true, 42, 42, false, true));
    QVERIFY(!detailPreviewMaySettleWithoutRerender(false, 42, 42, false, true));
    QVERIFY(!detailPreviewMaySettleWithoutRerender(true, 41, 42, false, true));
    QVERIFY(!detailPreviewMaySettleWithoutRerender(true, 42, 42, true, true));
    QVERIFY(!detailPreviewMaySettleWithoutRerender(true, 42, 42, false, false));
}

void CoreTests::interactiveRegionFallbackMatchesTiledRender()
{
    QImage source(389, 277, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor((x * 7 + y * 3) & 255,
                                              (x * 2 + y * 11) & 255,
                                              (x * 13 + y * 5) & 255,
                                              (x + y) & 255));
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode saturation;
    saturation.type = LayerType::Adjustment;
    saturation.adjustmentType = AdjustmentType::Saturation;
    SaturationParameters parameters;
    parameters.saturation = 37.0;
    saturation.setSaturationParameters(parameters);
    const QVector<LayerNode> layers {saturation, base};
    const QRect region(23, 19, 301, 211);

    TiledCanvasEngine engine;
    const TileCache::Stats cacheBefore = engine.cacheStats();
    TiledCanvasEngine::RenderInfo interactiveInfo;
    const QImage interactive = engine.renderInteractiveRegion(source,
                                                               layers,
                                                               region,
                                                               source.size(),
                                                               false,
                                                               0,
                                                               nullptr,
                                                               &interactiveInfo,
                                                               QUuid(),
                                                               0,
                                                               ColourProcessingCompatibility::LegacyV1,
                                                               true);
    const TileCache::Stats cacheAfterInteractive = engine.cacheStats();
    QCOMPARE(cacheAfterInteractive.residentTiles, cacheBefore.residentTiles);
    QCOMPARE(cacheAfterInteractive.dirtyTiles, cacheBefore.dirtyTiles);
    QCOMPARE(cacheAfterInteractive.hits, cacheBefore.hits);
    QCOMPARE(cacheAfterInteractive.misses, cacheBefore.misses);
    const QImage tiled = engine.renderRegion(source,
                                              layers,
                                              region,
                                              source.size(),
                                              false,
                                              0);
    QVERIFY(!interactive.isNull());
    QVERIFY(!tiled.isNull());
    QCOMPARE(interactive, tiled);
    QVERIFY(interactiveInfo.usedCpu);
    QVERIFY(!interactiveInfo.usedGpu);
    QCOMPARE(interactiveInfo.path,
             QStringLiteral("CPU exact bounded interactive compositor"));
}

void CoreTests::interactiveRegionFeatheredVectorMatchesTiledRenderWithoutPersistentTiles()
{
    VectorRasterizer::clearCache();
    QImage source(512, 384, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));

    LayerNode vector;
    vector.id = QUuid::createUuid();
    vector.type = LayerType::Vector;
    vector.name = QStringLiteral("Transient Feather Transform");
    VectorShape shape;
    shape.type = VectorShapeType::RoundedRectangle;
    shape.bounds = QRectF(148.0, 106.0, 116.0, 82.0);
    shape.cornerRadii.setAll(14.0);
    shape.fill.enabled = true;
    shape.fill.colour = QColor(224, 74, 42, 220);
    shape.stroke.enabled = true;
    shape.stroke.colour = QColor(31, 92, 218, 240);
    shape.stroke.width = 9.0;
    shape.stroke.alignment = VectorStrokeAlignment::Centre;
    shape.normalise();
    vector.vectorData.objects = {shape};
    vector.vectorData.featherRadius = 72.5;
    vector.vectorData.normalise();
    QVERIFY(vector.vectorData.isSafe());

    const QVector<LayerNode> layers {vector};
    const QRect region(58, 24, 332, 284);
    TiledCanvasEngine engine;
    const TileCache::Stats cacheBefore = engine.cacheStats();
    TiledCanvasEngine::RenderInfo interactiveInfo;
    const QImage interactive = engine.renderInteractiveRegion(
        source, layers, region, source.size(), false, 0, nullptr,
        &interactiveInfo, QUuid(), 0,
        ColourProcessingCompatibility::LegacyV1, true);
    const TileCache::Stats cacheAfterInteractive = engine.cacheStats();

    QVERIFY(!interactive.isNull());
    QCOMPARE(interactive.size(), region.size());
    QCOMPARE(cacheAfterInteractive.residentTiles, cacheBefore.residentTiles);
    QCOMPARE(cacheAfterInteractive.dirtyTiles, cacheBefore.dirtyTiles);
    QCOMPARE(cacheAfterInteractive.hits, cacheBefore.hits);
    QCOMPARE(cacheAfterInteractive.misses, cacheBefore.misses);
    QVERIFY(interactiveInfo.usedCpu);
    QVERIFY(!interactiveInfo.usedGpu);
    QCOMPARE(interactiveInfo.path,
             QStringLiteral("CPU exact bounded interactive compositor"));

    const QImage tiled = engine.renderRegion(source,
                                              layers,
                                              region,
                                              source.size(),
                                              false,
                                              0);
    QVERIFY(!tiled.isNull());
    QCOMPARE(interactive, tiled);
}

void CoreTests::progressivePreviewTilesCoverVisibleRegion()
{
    const QSize baseSize(1537, 1025);
    const QRect visible(221, 119, 911, 703);
    for (int level = 0; level <= ProgressivePreview::MaximumLevel; ++level) {
        const QVector<QRect> levelTiles = ProgressivePreview::levelTileRects(
            visible, baseSize, level, 0);
        QVERIFY(!levelTiles.isEmpty());

        QImage coverage(baseSize, QImage::Format_Grayscale8);
        coverage.fill(0);
        QPainter painter(&coverage);
        for (const QRect &levelTile : levelTiles) {
            const QRect baseTile = ProgressivePreview::baseRectForLevelTile(
                levelTile, baseSize, level);
            QVERIFY(!baseTile.isEmpty());
            painter.fillRect(baseTile, Qt::white);
        }
        painter.end();

        const QRect clipped = visible.intersected(QRect(QPoint(0, 0), baseSize));
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
            const uchar *row = coverage.constScanLine(y);
            for (int x = clipped.left(); x <= clipped.right(); ++x) {
                QVERIFY2(row[x] != 0, "Progressive tile coverage contained a gap");
            }
        }
    }
}


void CoreTests::progressiveLevelZeroAssemblyMatchesFullRender()
{
    QImage source(517, 391, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor((x * 5 + y * 3) & 255,
                                              (x * 2 + y * 7) & 255,
                                              (x * 11 + y) & 255,
                                              255));
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.opacity = 0.72;
    raster.blendMode = BlendMode::Screen;
    raster.rasterImage = QImage(source.size(), QImage::Format_RGBA8888);
    raster.rasterImage.fill(Qt::transparent);
    QPainter rasterPainter(&raster.rasterImage);
    rasterPainter.setPen(Qt::NoPen);
    rasterPainter.setBrush(QColor(210, 35, 160, 220));
    rasterPainter.drawEllipse(QRect(103, 47, 287, 251));
    rasterPainter.end();
    raster.transform = QTransform::fromTranslate(-13.0, 19.0);

    const QVector<LayerNode> layers {raster, base};
    const QImage expected = ImageProcessor::render(source,
                                                    layers,
                                                    nullptr,
                                                    source.size());
    QVERIFY(!expected.isNull());

    QImage assembled(source.size(), QImage::Format_ARGB32_Premultiplied);
    assembled.fill(Qt::transparent);
    assembled.setColorSpace(source.colorSpace());
    TiledCanvasEngine engine;
    QPainter painter(&assembled);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    const QVector<QRect> tiles = ProgressivePreview::levelTileRects(
        source.rect(), source.size(), 0, 0);
    for (const QRect &tileRect : tiles) {
        const QImage tile = engine.renderRegion(source,
                                                layers,
                                                tileRect,
                                                source.size(),
                                                false,
                                                0);
        QVERIFY(!tile.isNull());
        painter.drawImage(tileRect.topLeft(), tile);
    }
    painter.end();

    QCOMPARE(assembled.size(), expected.size());
    for (int y = 0; y < assembled.height(); ++y) {
        for (int x = 0; x < assembled.width(); ++x) {
            const QColor a = assembled.pixelColor(x, y);
            const QColor e = expected.pixelColor(x, y);
            QVERIFY(std::abs(a.red() - e.red()) <= 1);
            QVERIFY(std::abs(a.green() - e.green()) <= 1);
            QVERIFY(std::abs(a.blue() - e.blue()) <= 1);
            QVERIFY(std::abs(a.alpha() - e.alpha()) <= 1);
        }
    }
}

void CoreTests::multiresolutionTileMatchesCpuReference()
{
    QImage baseSource(513, 387, QImage::Format_RGBA8888);
    for (int y = 0; y < baseSource.height(); ++y) {
        for (int x = 0; x < baseSource.width(); ++x) {
            baseSource.setPixelColor(x, y, QColor((x * 3 + y) & 255,
                                                  (x + y * 5) & 255,
                                                  (x * 7 + y * 11) & 255,
                                                  255));
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.opacity = 0.65;
    raster.blendMode = BlendMode::Overlay;
    raster.rasterImage = QImage(baseSource.size(), QImage::Format_RGBA8888);
    raster.rasterImage.fill(Qt::transparent);
    QPainter rasterPainter(&raster.rasterImage);
    rasterPainter.fillRect(QRect(90, 70, 240, 180), QColor(240, 80, 20, 210));
    rasterPainter.end();
    raster.transform = QTransform::fromTranslate(17.0, -9.0);

    const QVector<LayerNode> layers {raster, base};
    const int level = 2;
    const QSize levelSize((baseSource.width() + 3) / 4,
                          (baseSource.height() + 3) / 4);
    QImage levelSource = baseSource.scaled(levelSize,
                                           Qt::IgnoreAspectRatio,
                                           Qt::SmoothTransformation);
    const QRect region(19, 13, 77, 61);
    const QImage expected = ImageProcessor::renderRegion(levelSource,
                                                          layers,
                                                          region,
                                                          baseSource.size());
    TiledCanvasEngine engine;
    const QImage actual = engine.renderRegion(levelSource,
                                               layers,
                                               region,
                                               baseSource.size(),
                                               false,
                                               level);
    QVERIFY(!expected.isNull());
    QVERIFY(!actual.isNull());
    QCOMPARE(actual.size(), expected.size());
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QColor a = actual.pixelColor(x, y);
            const QColor e = expected.pixelColor(x, y);
            QVERIFY(std::abs(a.red() - e.red()) <= 1);
            QVERIFY(std::abs(a.green() - e.green()) <= 1);
            QVERIFY(std::abs(a.blue() - e.blue()) <= 1);
            QVERIFY(std::abs(a.alpha() - e.alpha()) <= 1);
        }
    }
}

void CoreTests::nativeGpuTileRoundTripMatchesCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }

    QImage source(31, 19, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor((x * 23 + y) & 255,
                                              (x + y * 31) & 255,
                                              (x * 7 + y * 9) & 255,
                                              (x * 17 + y * 13) & 255));
        }
    }

    QString error;
    const QImage result = backend.roundTripDiagnosticTile(source, &error);
    QVERIFY2(!result.isNull(), qPrintable(error));
    QCOMPARE(result.size(), source.size());
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            const QColor expected = source.pixelColor(x, y);
            const QColor actual = result.pixelColor(x, y);
            QVERIFY(std::abs(expected.red() - actual.red()) <= 1);
            QVERIFY(std::abs(expected.green() - actual.green()) <= 1);
            QVERIFY(std::abs(expected.blue() - actual.blue()) <= 1);
            QVERIFY(std::abs(expected.alpha() - actual.alpha()) <= 1);
        }
    }
}


void CoreTests::nativeGpuImageResizeMatchesCpuAcrossTileBoundariesWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(
        QStringLiteral("GPU resize parity approved for test"), true);

    QImage source(517, 259, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4] = static_cast<uchar>((x * 17 + y * 3 + 11) & 255);
            row[x * 4 + 1] = static_cast<uchar>((x * 5 + y * 23 + 37) & 255);
            row[x * 4 + 2] = static_cast<uchar>((x * 13 + y * 7 + 91) & 255);
            row[x * 4 + 3] = static_cast<uchar>((x + y) % 7 == 0
                                                    ? 0
                                                    : ((x * 19 + y * 29) & 255));
        }
    }
    const QSize destinationSize(773, 387);
    const QImage expected = resampleStraightRgbaCpuReference(
        source, destinationSize, ImageResampleMethod::Bilinear);
    QString error;
    const QImage actual = backend.resampleImageTiled(
        source, destinationSize, ImageResampleMethod::Bilinear, nullptr, &error);
    QVERIFY2(!actual.isNull(), qPrintable(error));
    QCOMPARE(actual.size(), expected.size());

    int maximumDifference = 0;
    for (int y = 0; y < actual.height(); ++y) {
        const uchar *actualRow = actual.constScanLine(y);
        const uchar *expectedRow = expected.constScanLine(y);
        for (int x = 0; x < actual.width() * 4; ++x) {
            maximumDifference = std::max(
                maximumDifference,
                std::abs(int(actualRow[x]) - int(expectedRow[x])));
        }
    }
    QVERIFY2(maximumDifference <= 1,
             qPrintable(QStringLiteral("Maximum resize CPU/GPU delta was %1")
                            .arg(maximumDifference)));

    QImage tieSource(2, 1, QImage::Format_RGBA8888);
    tieSource.setPixelColor(0, 0, QColor(3, 7, 11, 0));
    tieSource.setPixelColor(1, 0, QColor(193, 197, 211, 255));
    const QImage expectedTie = resampleStraightRgbaCpuReference(
        tieSource, QSize(49, 1), ImageResampleMethod::NearestNeighbour);
    error.clear();
    const QImage actualTie = backend.resampleImageTiled(
        tieSource, QSize(49, 1), ImageResampleMethod::NearestNeighbour,
        nullptr, &error);
    QVERIFY2(!actualTie.isNull(), qPrintable(error));
    QCOMPARE(actualTie, expectedTie);

    QImage mask(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            row[x] = static_cast<uchar>((x * 31 + y * 17 + 9) & 255);
        }
    }
    const QImage expectedMask = resampleGrayscaleCpuReference(
        mask, destinationSize, ImageResampleMethod::Bilinear);
    error.clear();
    const QImage actualMask = backend.resampleImageTiled(
        mask, destinationSize, ImageResampleMethod::Bilinear, nullptr, &error);
    QVERIFY2(!actualMask.isNull(), qPrintable(error));
    QCOMPARE(actualMask.format(), QImage::Format_Grayscale8);
    int maximumMaskDifference = 0;
    for (int y = 0; y < actualMask.height(); ++y) {
        const uchar *actualRow = actualMask.constScanLine(y);
        const uchar *expectedRow = expectedMask.constScanLine(y);
        for (int x = 0; x < actualMask.width(); ++x) {
            maximumMaskDifference = std::max(
                maximumMaskDifference,
                std::abs(int(actualRow[x]) - int(expectedRow[x])));
        }
    }
    QVERIFY2(maximumMaskDifference <= 1,
             qPrintable(QStringLiteral("Maximum mask resize CPU/GPU delta was %1")
                            .arg(maximumMaskDifference)));
}


void CoreTests::nativeGpuAdjustmentsMatchCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuCompositorParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }

    // A human-readable partial-success message must not approve native document
    // work. Only the helper's explicit PASS result is authoritative.
    backend.setExternalDiagnosticStatus(
        QStringLiteral("Native WebGPU tile parity passed, but adjustment validation failed"),
        false);
    QImage gateSource(3, 3, QImage::Format_RGBA8888);
    gateSource.fill(QColor(40, 80, 120, 255));
    LayerNode gateBase;
    gateBase.type = LayerType::BaseImage;
    QVERIFY(!backend.renderRegion(gateSource,
                                  {gateBase},
                                  gateSource.rect(),
                                  gateSource.size(),
                                  0).isNull());
    QVERIFY(backend.statusText().contains(QStringLiteral("Last operation path: CPU tiled reference compositor")));

    backend.setExternalDiagnosticStatus(QStringLiteral("GPU parity passed for test"), true);

    // Deliberately span several 256-pixel tile boundaries so the native test
    // exercises independent adjustment command streams and their assembly.
    QImage source(540, 300, QImage::Format_RGBA8888);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor((17 + x * 9 + y * 3) & 255,
                                              (31 + x * 5 + y * 7) & 255,
                                              (67 + x * 2 + y * 11) & 255,
                                              (x * 13 + y * 17) & 255));
        }
    }

    QImage mask(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            row[x] = static_cast<uchar>(std::clamp(32 + x * 2 + y, 0, 255));
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 0.6;
    exposure.opacity = 0.75;
    exposure.maskImage = mask;
    exposure.maskInverted = true;

    LayerNode contrast;
    contrast.type = LayerType::Adjustment;
    contrast.adjustmentType = AdjustmentType::Contrast;
    contrast.contrast = 27.0;
    contrast.opacity = 0.8;
    contrast.blendMode = BlendMode::Overlay;

    LayerNode saturation;
    saturation.type = LayerType::Adjustment;
    saturation.adjustmentType = AdjustmentType::Saturation;
    saturation.saturation = -35.0;

    LayerNode levels;
    levels.type = LayerType::Adjustment;
    LevelsParameters gpuLevels;
    auto &gpuMaster = gpuLevels.channel(AdjustmentChannel::Rgb);
    gpuMaster.inputBlack = 0.06;
    gpuMaster.inputWhite = 0.93;
    gpuMaster.gamma = 1.18;
    gpuMaster.outputBlack = 0.025;
    gpuMaster.outputWhite = 0.97;
    auto &gpuRed = gpuLevels.channel(AdjustmentChannel::Red);
    gpuRed.inputBlack = 0.035;
    gpuRed.inputWhite = 0.91;
    gpuRed.gamma = 0.86;
    gpuRed.outputBlack = 0.04;
    gpuRed.outputWhite = 0.88;
    auto &gpuGreen = gpuLevels.channel(AdjustmentChannel::Green);
    gpuGreen.gamma = 1.27;
    gpuGreen.outputBlack = 0.02;
    gpuGreen.outputWhite = 0.94;
    auto &gpuBlue = gpuLevels.channel(AdjustmentChannel::Blue);
    gpuBlue.inputBlack = 0.08;
    gpuBlue.inputWhite = 0.96;
    gpuBlue.outputBlack = 0.11;
    gpuBlue.outputWhite = 0.98;
    levels.setLevelsParameters(gpuLevels);
    levels.opacity = 0.9;

    LayerNode curves;
    curves.type = LayerType::Adjustment;
    CurvesParameters gpuCurves;
    gpuCurves.channel(AdjustmentChannel::Rgb).points = {
        {0.0, 0.0}, {0.23, 0.16}, {0.64, 0.72}, {1.0, 1.0}
    };
    gpuCurves.channel(AdjustmentChannel::Red).points = {
        {0.0, 0.02}, {0.45, 0.53}, {1.0, 0.98}
    };
    curves.setCurvesParameters(gpuCurves);
    curves.opacity = 0.83;

    LayerNode hueSaturation;
    hueSaturation.type = LayerType::Adjustment;
    HueSaturationParameters gpuHueSaturation;
    gpuHueSaturation.hue = 9.0;
    gpuHueSaturation.saturation = 16.0;
    gpuHueSaturation.range(HueSaturationRange::Reds).hue = -18.0;
    gpuHueSaturation.range(HueSaturationRange::Reds).saturation = 27.0;
    gpuHueSaturation.range(HueSaturationRange::Blues).lightness = -11.0;
    hueSaturation.setHueSaturationParameters(gpuHueSaturation);
    hueSaturation.opacity = 0.78;

    LayerNode vibrance;
    vibrance.type = LayerType::Adjustment;
    vibrance.setVibranceParameters({47.0, -8.0, 76.0});
    vibrance.opacity = 0.74;

    LayerNode whiteBalance;
    whiteBalance.type = LayerType::Adjustment;
    whiteBalance.setWhiteBalanceParameters({19.0, -12.0});
    whiteBalance.opacity = 0.71;

    LayerNode colourBalance;
    colourBalance.type = LayerType::Adjustment;
    ColourBalanceParameters gpuColourBalance;
    gpuColourBalance.range(ColourBalanceRange::Shadows).cyanRed = -13.0;
    gpuColourBalance.range(ColourBalanceRange::Midtones).magentaGreen = 17.0;
    gpuColourBalance.range(ColourBalanceRange::Highlights).yellowBlue = -15.0;
    colourBalance.setColourBalanceParameters(gpuColourBalance);
    colourBalance.opacity = 0.72;

    LayerNode gradientMap;
    gradientMap.type = LayerType::Adjustment;
    GradientMapParameters gpuGradientMap;
    gpuGradientMap.stops = {
        {0.0, QColor(QStringLiteral("#10192d"))},
        {0.45, QColor(QStringLiteral("#7b5260"))},
        {1.0, QColor(QStringLiteral("#f6d8a9"))}
    };
    gpuGradientMap.interpolation = GradientInterpolation::Smooth;
    gradientMap.setGradientMapParameters(gpuGradientMap);
    gradientMap.opacity = 0.31;

    LayerNode group;
    group.type = LayerType::Group;
    group.opacity = 0.85;
    group.children = {gradientMap, colourBalance, whiteBalance, vibrance,
                      hueSaturation, curves, levels, saturation, contrast,
                      exposure, base};

    const QVector<LayerNode> layers {group};
    const QRect region(200, 18, 325, 264);
    const QImage expected = ImageProcessor::renderRegion(source,
                                                          layers,
                                                          region,
                                                          source.size());
    const QImage actual = backend.renderRegion(source,
                                                layers,
                                                region,
                                                source.size(),
                                                0);
    QVERIFY(!expected.isNull());
    QVERIFY(!actual.isNull());
    QVERIFY2(backend.statusText().contains(
                 QStringLiteral("Last operation path: Native WebGPU")),
             qPrintable(backend.statusText()));
    QCOMPARE(actual.size(), expected.size());
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QColor a = actual.pixelColor(x, y);
            const QColor e = expected.pixelColor(x, y);
            QVERIFY(std::abs(a.red() - e.red()) <= 2);
            QVERIFY(std::abs(a.green() - e.green()) <= 2);
            QVERIFY(std::abs(a.blue() - e.blue()) <= 2);
            QVERIFY(std::abs(a.alpha() - e.alpha()) <= 2);
        }
    }
}


void CoreTests::nativeGpuLiveFilterStackMatchesCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuCompositorParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    const quint32 exposureBit = quint32(1) << static_cast<quint32>(AdjustmentType::Exposure);
    const quint32 vibranceBit = quint32(1) << static_cast<quint32>(AdjustmentType::Vibrance);
    const quint32 requiredMask = exposureBit | vibranceBit;
    const quint32 approvedMask = backend.diagnosticCapabilities().webGpu.approvedAdjustmentMask;
    if ((approvedMask & requiredMask) != requiredMask) {
        QSKIP("Exposure/Vibrance native adjustment kernels did not pass startup parity");
    }
    backend.setExternalDiagnosticStatus(
        QStringLiteral("GPU Live Filter parity approved for test"), true);

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Native Live Filter Stack");
    settings.pixelSize = QSize(420, 286);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    QVERIFY(document.updateLayer(document.baseLayerId(), [](LayerNode &layer) {
        layer.visible = false;
    }));

    const QUuid rasterId = document.addRasterLayer();
    QImage pixels(settings.pixelSize, QImage::Format_RGBA8888);
    pixels.fill(Qt::transparent);
    pixels.setColorSpace(document.sourceImage().colorSpace());
    for (int y = 0; y < pixels.height(); ++y) {
        for (int x = 0; x < pixels.width(); ++x) {
            pixels.setPixelColor(x, y, QColor((31 + x * 3 + y) & 255,
                                              (67 + x + y * 5) & 255,
                                              (109 + x * 7 + y * 2) & 255,
                                              (80 + x * 11 + y * 13) & 255));
        }
    }
    QVERIFY(document.updateLayer(rasterId, [&](LayerNode &layer) {
        layer.rasterImage = pixels;
    }));
    const QUuid smartId = document.convertLayersToEmbeddedSmart({rasterId}, &error);
    QVERIFY2(!smartId.isNull(), qPrintable(error));
    QVERIFY(document.updateLayer(smartId, [](LayerNode &layer) {
        QTransform transform;
        transform.translate(11.5, -7.25);
        transform.rotate(7.0);
        transform.scale(0.93, 1.04);
        layer.transform = transform;
        layer.smartTransform.interpolation = TransformInterpolation::Bilinear;
    }));

    const QUuid exposureId = document.addLiveFilter(
        smartId, AdjustmentType::Exposure, &error);
    QVERIFY2(!exposureId.isNull(), qPrintable(error));
    AdjustmentData exposure;
    exposure.reset(AdjustmentType::Exposure);
    auto exposureParameters = std::get<ExposureParameters>(exposure.parameters);
    exposureParameters.exposure = 0.55;
    exposure.parameters = exposureParameters;
    QVERIFY(document.updateLiveFilter(smartId, exposureId, exposure));

    const QUuid vibranceId = document.addLiveFilter(
        smartId, AdjustmentType::Vibrance, &error);
    QVERIFY2(!vibranceId.isNull(), qPrintable(error));
    AdjustmentData vibrance;
    vibrance.reset(AdjustmentType::Vibrance);
    auto vibranceParameters = std::get<VibranceParameters>(vibrance.parameters);
    vibranceParameters.vibrance = 42.0;
    vibranceParameters.saturation = -9.0;
    vibrance.parameters = vibranceParameters;
    QVERIFY(document.updateLiveFilter(smartId, vibranceId, vibrance));

    QVERIFY(document.addLiveFilterMask(smartId, vibranceId));
    QImage mask(settings.pixelSize, QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            row[x] = static_cast<uchar>(std::clamp((x * 255) / std::max(1, mask.width() - 1),
                                                   0, 255));
        }
    }
    QVERIFY(document.updateLiveFilterMask(smartId, vibranceId, mask,
                                          settings.pixelSize, QPointF()));

    const QRect region(118, 22, 287, 244);
    const QImage expected = ImageProcessor::renderRegion(
        document.sourceImage(), document.layers(), region,
        document.sourceImage().size(), nullptr,
        document.colourState().processingCompatibility);
    TiledCanvasEngine::RenderInfo info;
    const QImage actual = backend.renderRegion(
        document.sourceImage(), document.layers(), region,
        document.sourceImage().size(), 0, nullptr, &info);
    QVERIFY(!expected.isNull());
    QVERIFY(!actual.isNull());
    QVERIFY2(info.usedGpu, qPrintable(backend.statusText()));
    QCOMPARE(actual.size(), expected.size());
    int maximumDifference = 0;
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QColor a = actual.pixelColor(x, y);
            const QColor e = expected.pixelColor(x, y);
            maximumDifference = std::max({maximumDifference,
                                          std::abs(a.red() - e.red()),
                                          std::abs(a.green() - e.green()),
                                          std::abs(a.blue() - e.blue()),
                                          std::abs(a.alpha() - e.alpha())});
        }
    }
    QVERIFY2(maximumDifference <= 2,
             qPrintable(QStringLiteral("Maximum Live Filter CPU/GPU difference was %1")
                            .arg(maximumDifference)));
}


void CoreTests::nativeGpuShadowsHighlightsMatchesCpuAcrossTileBoundariesWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuCompositorParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(
        QStringLiteral("GPU parity passed for Shadows/Highlights test"), true);

    const QSize size(620, 390);
    QImage source(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const bool darkZone = x < 270;
            const bool brightZone = y < 205;
            source.setPixelColor(x, y, QColor(
                darkZone ? 18 + (x + y) % 61 : 188 + (x * 3 + y) % 58,
                brightZone ? 176 + (x + y * 2) % 71 : 24 + (x * 2 + y) % 73,
                43 + (x * 5 + y * 7) % 181,
                41 + (x * 11 + y * 13) % 215));
        }
    }
    QImage mask(size, QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x] = static_cast<uchar>((x + y * 2) % 256);
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();
    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    ShadowsHighlightsParameters parameters;
    parameters.shadowAmount = 52.0;
    parameters.shadowTonalWidth = 67.0;
    parameters.highlightAmount = 43.0;
    parameters.highlightTonalWidth = 59.0;
    parameters.radius = 73.0;
    parameters.midtoneContrast = 14.0;
    parameters.colourCorrection = 26.0;
    adjustment.setShadowsHighlightsParameters(parameters);
    adjustment.opacity = 0.82;
    adjustment.maskImage = mask;
    adjustment.maskReferenceSize = size;
    adjustment.maskEnabled = true;

    const QVector<LayerNode> layers {adjustment, base};
    const QRect region(181, 117, 347, 239);
    const QImage expected = ImageProcessor::renderRegion(
        source, layers, region, size);
    const QImage actual = backend.renderRegion(source, layers, region, size, 0);
    QVERIFY(!expected.isNull());
    QVERIFY(!actual.isNull());
    QCOMPARE(actual.size(), expected.size());
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QColor gpu = actual.pixelColor(x, y);
            const QColor cpu = expected.pixelColor(x, y);
            QVERIFY2(std::abs(gpu.red() - cpu.red()) <= 2,
                     qPrintable(QStringLiteral("red mismatch at %1,%2: %3 vs %4")
                                    .arg(x).arg(y).arg(gpu.red()).arg(cpu.red())));
            QVERIFY(std::abs(gpu.green() - cpu.green()) <= 2);
            QVERIFY(std::abs(gpu.blue() - cpu.blue()) <= 2);
            QVERIFY(std::abs(gpu.alpha() - cpu.alpha()) <= 2);
        }
    }
    QVERIFY(backend.statusText().contains(
        QStringLiteral("Last operation path: Native WebGPU")));
}

void CoreTests::nativeGpuPassThroughMatchesCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuCompositorParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(QStringLiteral("GPU parity passed for Pass Through test"),
                                        true);

    QImage source(540, 300, QImage::Format_RGBA8888);
    QImage raster(source.size(), QImage::Format_RGBA8888);
    QImage mask(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < source.height(); ++y) {
        uchar *maskRow = mask.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, QColor((23 + x * 7 + y * 3) & 255,
                                              (41 + x * 2 + y * 9) & 255,
                                              (89 + x * 11 + y * 5) & 255,
                                              255));
            raster.setPixelColor(x, y, QColor((201 + x * 3 + y) & 255,
                                              (37 + x * 5 + y * 7) & 255,
                                              (113 + x * 2 + y * 13) & 255,
                                              211));
            maskRow[x] = static_cast<uchar>((17 + x * 3 + y * 5) & 255);
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode childRaster;
    childRaster.type = LayerType::Raster;
    childRaster.rasterImage = raster;
    childRaster.opacity = 0.37;
    childRaster.blendMode = BlendMode::Screen;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.adjustmentType = AdjustmentType::Exposure;
    exposure.exposure = 0.8;

    LayerNode containedContrast;
    containedContrast.type = LayerType::Adjustment;
    containedContrast.adjustmentType = AdjustmentType::Contrast;
    containedContrast.contrast = 19.0;

    LayerNode containedPass;
    containedPass.type = LayerType::Group;
    containedPass.groupCompositeMode = GroupCompositeMode::PassThrough;
    containedPass.children = {containedContrast};
    containedPass.opacity = 0.76;
    containedPass.maskImage = mask;

    LayerNode isolated;
    isolated.type = LayerType::Group;
    isolated.groupCompositeMode = GroupCompositeMode::Isolated;
    isolated.children = {containedPass, childRaster};
    isolated.opacity = 0.83;

    LayerNode inner;
    inner.type = LayerType::Group;
    inner.groupCompositeMode = GroupCompositeMode::PassThrough;
    inner.children = {exposure, isolated};

    LayerNode outer;
    outer.type = LayerType::Group;
    outer.groupCompositeMode = GroupCompositeMode::PassThrough;
    outer.children = {inner};
    outer.opacity = 0.61;
    outer.maskImage = mask;
    outer.maskInverted = true;

    const QRect region(180, 11, 347, 278);

    // A two-level Isolated/Pass Through combination has a dedicated passing
    // startup case and must remain native even when the deeper mixed-hierarchy
    // edge case is routed through the CPU reference.
    const QVector<LayerNode> shallowLayers {isolated, base};
    const QImage expectedShallow = ImageProcessor::renderRegion(source,
                                                                 shallowLayers,
                                                                 region,
                                                                 source.size());
    TiledCanvasEngine::RenderInfo shallowInfo;
    const QImage actualShallow = backend.renderRegion(source,
                                                       shallowLayers,
                                                       region,
                                                       source.size(),
                                                       0,
                                                       nullptr,
                                                       &shallowInfo);
    QVERIFY(!expectedShallow.isNull());
    QVERIFY(!actualShallow.isNull());
    QVERIFY(shallowInfo.usedGpu);
    QCOMPARE(actualShallow.size(), expectedShallow.size());

    const QVector<LayerNode> layers {outer, base};
    const QImage expected = ImageProcessor::renderRegion(source,
                                                          layers,
                                                          region,
                                                          source.size());
    TiledCanvasEngine::RenderInfo info;
    const QImage actual = backend.renderRegion(source,
                                                layers,
                                                region,
                                                source.size(),
                                                0,
                                                nullptr,
                                                &info);
    QVERIFY(!expected.isNull());
    QVERIFY(!actual.isNull());
    QCOMPARE(actual.size(), expected.size());
    if (backend.webGpuFullCompositorParityPassed()) {
        QVERIFY(info.usedGpu);
    } else {
        QVERIFY(!info.usedGpu);
        QVERIFY(info.path.startsWith(QStringLiteral("CPU tiled reference")));
    }
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QColor rendered = actual.pixelColor(x, y);
            const QColor cpu = expected.pixelColor(x, y);
            const int tolerance = info.usedGpu ? 2 : 0;
            QVERIFY(std::abs(rendered.red() - cpu.red()) <= tolerance);
            QVERIFY(std::abs(rendered.green() - cpu.green()) <= tolerance);
            QVERIFY(std::abs(rendered.blue() - cpu.blue()) <= tolerance);
            QVERIFY(std::abs(rendered.alpha() - cpu.alpha()) <= tolerance);
        }
    }
}


void CoreTests::nativeHierarchyResourceGuardFallsBackSafelyWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(QStringLiteral("GPU approved for resource guard test"),
                                        true);

    QImage source(4, 4, QImage::Format_RGBA8888);
    source.fill(QColor(70, 80, 90, 255));
    LayerNode base;
    base.type = LayerType::BaseImage;

    QVector<LayerNode> layers;
    layers.reserve(1026);
    for (int index = 0; index < 1025; ++index) {
        LayerNode adjustment;
        adjustment.type = LayerType::Adjustment;
        adjustment.adjustmentType = AdjustmentType::Exposure;
        adjustment.exposure = (index % 3 == 0) ? 0.01 : 0.0;
        layers.push_back(std::move(adjustment));
    }
    layers.push_back(base);

    TiledCanvasEngine::RenderInfo info;
    const QImage rendered = backend.renderRegion(source,
                                                 layers,
                                                 source.rect(),
                                                 source.size(),
                                                 0,
                                                 nullptr,
                                                 &info);
    QVERIFY(!rendered.isNull());
    QVERIFY(!info.usedGpu);
    QVERIFY(info.path.startsWith(QStringLiteral("CPU tiled reference")));
    QVERIFY(info.fallbackReason.contains(QStringLiteral("resource guard"),
                                         Qt::CaseInsensitive));

    // Exercise the working-texture estimate independently of the node limit.
    // The raster and mask payloads are implicitly shared, so this remains a
    // modest test allocation while representing a wide native command graph.
    QImage memorySource(256, 256, QImage::Format_RGBA8888);
    memorySource.fill(QColor(41, 52, 63, 255));
    QImage compactMask(1, 1, QImage::Format_Grayscale8);
    compactMask.fill(127);
    LayerNode memoryBase;
    memoryBase.type = LayerType::BaseImage;
    QVector<LayerNode> memoryLayers;
    memoryLayers.reserve(351);
    for (int index = 0; index < 350; ++index) {
        LayerNode raster;
        raster.type = LayerType::Raster;
        raster.rasterImage = memorySource;
        raster.maskImage = compactMask;
        raster.opacity = 0.01;
        memoryLayers.push_back(std::move(raster));
    }
    memoryLayers.push_back(memoryBase);

    TiledCanvasEngine::RenderInfo memoryInfo;
    const QImage memoryRendered = backend.renderRegion(memorySource,
                                                       memoryLayers,
                                                       memorySource.rect(),
                                                       memorySource.size(),
                                                       0,
                                                       nullptr,
                                                       &memoryInfo);
    QVERIFY(!memoryRendered.isNull());
    QVERIFY(!memoryInfo.usedGpu);
    QVERIFY(memoryInfo.fallbackReason.contains(
        QStringLiteral("estimated working textures"), Qt::CaseInsensitive));
}

void CoreTests::alphaChannelStrokePreservesRgb()
{
    const QSize size(32, 32);
    QImage source(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>((x * 7 + y * 3) & 255);
            row[x * 4 + 1] = static_cast<uchar>((x * 5 + y * 11) & 255);
            row[x * 4 + 2] = static_cast<uchar>((x * 13 + y) & 255);
            row[x * 4 + 3] = 255;
        }
    }

    TiledCanvasEngine engine;
    const auto result = engine.stampChannelStroke(source,
                                                   size,
                                                   source.colorSpace(),
                                                   QUuid::createUuid(),
                                                   1,
                                                   {QLineF(QPointF(16.0, 16.0),
                                                           QPointF(16.0, 16.0))},
                                                   QTransform(),
                                                   12.0,
                                                   1.0,
                                                   0.99,
                                                   3,
                                                   0);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    const QImage painted = result.image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        const uchar *before = source.constScanLine(y);
        const uchar *after = painted.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(after[x * 4 + 0], before[x * 4 + 0]);
            QCOMPARE(after[x * 4 + 1], before[x * 4 + 1]);
            QCOMPARE(after[x * 4 + 2], before[x * 4 + 2]);
        }
    }
    QCOMPARE(painted.constScanLine(16)[16 * 4 + 3], static_cast<uchar>(0));
}

void CoreTests::rgbaChannelStrokeTouchesOnlySelectedComponent()
{
    const QSize size(24, 24);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(QColor(20, 70, 130, 190));

    TiledCanvasEngine engine;
    for (int channel = 0; channel < 4; ++channel) {
        const auto result = engine.stampChannelStroke(source,
                                                       size,
                                                       source.colorSpace(),
                                                       QUuid::createUuid(),
                                                       1,
                                                       {QLineF(QPointF(12.0, 12.0),
                                                               QPointF(12.0, 12.0))},
                                                       QTransform(),
                                                       10.0,
                                                       1.0,
                                                       0.99,
                                                       channel,
                                                       240);
        QVERIFY2(!result.image.isNull(), qPrintable(result.error));
        const QImage painted = result.image.convertToFormat(QImage::Format_RGBA8888);
        const uchar *before = source.constScanLine(12) + 12 * 4;
        const uchar *after = painted.constScanLine(12) + 12 * 4;
        for (int component = 0; component < 4; ++component) {
            if (component == channel) {
                QCOMPARE(after[component], static_cast<uchar>(240));
            } else {
                QCOMPARE(after[component], before[component]);
            }
        }
    }
}

void CoreTests::sixteenBitChannelStrokePreservesOtherComponents()
{
    const QSize size(20, 20);
    QImage source(size, QImage::Format_RGBA64);
    for (int y = 0; y < size.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            row[x] = QRgba64::fromRgba64(50001, 31002, 12003, 65535);
        }
    }

    TiledCanvasEngine engine;
    const auto result = engine.stampChannelStroke(source,
                                                   size,
                                                   source.colorSpace(),
                                                   QUuid::createUuid(),
                                                   1,
                                                   {QLineF(QPointF(10.0, 10.0),
                                                           QPointF(10.0, 10.0))},
                                                   QTransform(),
                                                   8.0,
                                                   1.0,
                                                   0.99,
                                                   3,
                                                   0);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    const QImage painted = result.image.convertToFormat(QImage::Format_RGBA64);
    for (int y = 0; y < size.height(); ++y) {
        const auto *before = reinterpret_cast<const QRgba64 *>(source.constScanLine(y));
        const auto *after = reinterpret_cast<const QRgba64 *>(painted.constScanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(after[x].red(), before[x].red());
            QCOMPARE(after[x].green(), before[x].green());
            QCOMPARE(after[x].blue(), before[x].blue());
        }
    }
    QCOMPARE(reinterpret_cast<const QRgba64 *>(painted.constScanLine(10))[10].alpha(),
             static_cast<quint16>(0));
}

void CoreTests::channelTileDeltaRoundTripsExactly()
{
    QImage before(700, 530, QImage::Format_RGBA8888);
    before.fill(QColor(24, 48, 72, 255));
    QImage after = before;
    after.detach();
    for (int y = 180; y < 300; ++y) {
        uchar *row = after.scanLine(y);
        for (int x = 245; x < 435; ++x) {
            row[x * 4 + 3] = static_cast<uchar>((x + y) & 255);
        }
    }

    const QRect affected(245, 180, 190, 120);
    const ChannelTileDeltaSet delta = buildChannelTileDeltaSet(before,
                                                                after,
                                                                affected,
                                                                3,
                                                                256);
    QVERIFY(!delta.isEmpty());
    QCOMPARE(delta.channelIndex, 3);
    QCOMPARE(delta.bytesPerChannel, 1);

    bool ok = false;
    const QImage undone = applyChannelTileDeltaSet(after, delta, false, &ok);
    QVERIFY(ok);
    QCOMPARE(undone, before);
    const QImage redone = applyChannelTileDeltaSet(undone, delta, true, &ok);
    QVERIFY(ok);
    QCOMPARE(redone, after);
}

void CoreTests::channelTileDeltaUsesComponentStorage()
{
    QImage before(2048, 2048, QImage::Format_RGBA64);
    before.fill(QColor(40, 80, 120, 255));
    QImage after = before;
    after.detach();
    auto *row = reinterpret_cast<QRgba64 *>(after.scanLine(900));
    const QRgba64 pixel = row[900];
    row[900] = QRgba64::fromRgba64(pixel.red(), pixel.green(), pixel.blue(), 0);

    const ChannelTileDeltaSet delta = buildChannelTileDeltaSet(before,
                                                                after,
                                                                QRect(900, 900, 1, 1),
                                                                3,
                                                                256);
    QCOMPARE(delta.tiles.size(), 1);
    QCOMPARE(delta.bytesPerChannel, 2);
    QCOMPARE(delta.tiles.constFirst().rawByteCount, 256 * 256 * 2);
    QVERIFY(delta.storedBytes() < before.sizeInBytes() / 64);
}

void CoreTests::rasterTileDeltaRoundTripsExactly()
{
    QImage before(700, 530, QImage::Format_ARGB32_Premultiplied);
    before.fill(QColor(24, 48, 72, 255));
    QImage after = before;
    QPainter painter(&after);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.fillRect(QRect(245, 180, 190, 120), QColor(220, 35, 90, 173));
    painter.end();

    const QRect affected(245, 180, 190, 120);
    const RasterTileDeltaSet delta = buildRasterTileDeltaSet(before, after, affected, 256);
    QVERIFY(!delta.isEmpty());

    bool ok = false;
    const QImage undone = applyRasterTileDeltaSet(after, delta, false, &ok);
    QVERIFY(ok);
    QCOMPARE(undone, before);

    const QImage redone = applyRasterTileDeltaSet(undone, delta, true, &ok);
    QVERIFY(ok);
    QCOMPARE(redone, after);

    QImage toggled = redone;
    for (int iteration = 0; iteration < 20; ++iteration) {
        const bool targetAfter = (iteration % 2) != 0;
        toggled = applyRasterTileDeltaSet(toggled, delta, targetAfter, &ok);
        QVERIFY(ok);
        QCOMPARE(toggled, targetAfter ? after : before);
    }
}

void CoreTests::rasterTileDeltaPreservesStraightRgba64AndHiddenRgb()
{
    QImage before(QSize(260, 258), QImage::Format_RGBA64);
    before.fill(Qt::transparent);
    before.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    auto *beforeRow = reinterpret_cast<QRgba64 *>(before.scanLine(257));
    beforeRow[259] = QRgba64::fromRgba64(54321, 12345, 40001, 0);

    QImage after = before;
    auto *afterRow = reinterpret_cast<QRgba64 *>(after.scanLine(257));
    afterRow[259] = QRgba64::fromRgba64(54321, 12345, 40001, 32768);

    const RasterTileDeltaSet delta = buildRasterTileDeltaSet(
        before, after, QRect(259, 257, 1, 1), 256);
    QVERIFY(!delta.isEmpty());
    QCOMPARE(delta.bytesPerPixel, 8);
    QCOMPARE(delta.storageFormat, QImage::Format_RGBA64);

    bool ok = false;
    const QImage undone = applyRasterTileDeltaSet(after, delta, false, &ok);
    QVERIFY(ok);
    QVERIFY(exactImagesEqual(undone, before));
    const auto *undoneRow = reinterpret_cast<const QRgba64 *>(undone.constScanLine(257));
    QCOMPARE(undoneRow[259].red(), static_cast<quint16>(54321));
    QCOMPARE(undoneRow[259].green(), static_cast<quint16>(12345));
    QCOMPARE(undoneRow[259].blue(), static_cast<quint16>(40001));
    QCOMPARE(undoneRow[259].alpha(), static_cast<quint16>(0));

    const QImage redone = applyRasterTileDeltaSet(undone, delta, true, &ok);
    QVERIFY(ok);
    QVERIFY(exactImagesEqual(redone, after));
}

void CoreTests::rasterTileDeltaStoresOnlyChangedTiles()
{
    QImage before(2048, 2048, QImage::Format_ARGB32_Premultiplied);
    before.fill(Qt::transparent);
    QImage after = before;
    after.setPixelColor(900, 900, QColor(255, 255, 255, 255));

    const RasterTileDeltaSet delta = buildRasterTileDeltaSet(before,
                                                              after,
                                                              before.rect(),
                                                              256);
    QCOMPARE(delta.tiles.size(), 1);
    QVERIFY(delta.storedBytes() < before.sizeInBytes() / 32);
    QVERIFY(delta.tiles.constFirst().compressed);
    QVERIFY(delta.tiles.constFirst().rect.contains(QPoint(900, 900)));
}

void CoreTests::rasterTileDeltaSparseRegionsRoundTrip()
{
    QImage before(1024, 1024, QImage::Format_RGBA8888);
    before.fill(Qt::transparent);
    QImage after = before;
    after.detach();
    after.setPixelColor(20, 20, QColor(220, 40, 80, 255));
    after.setPixelColor(900, 900, QColor(20, 190, 240, 180));

    const QVector<QRect> sparseRegions {
        QRect(12, 12, 24, 24),
        QRect(892, 892, 24, 24)
    };
    const RasterTileDeltaSet delta = buildRasterTileDeltaSet(before,
                                                              after,
                                                              sparseRegions,
                                                              256);
    QCOMPARE(delta.tiles.size(), 2);
    bool ok = false;
    const QImage undone = applyRasterTileDeltaSet(after, delta, false, &ok);
    QVERIFY(ok);
    QCOMPARE(undone, before);
    const QImage redone = applyRasterTileDeltaSet(undone, delta, true, &ok);
    QVERIFY(ok);
    QCOMPARE(redone, after);
}


void CoreTests::rasterTileDeltaTracksBoundaryTiles()
{
    QImage before(2048, 2048, QImage::Format_ARGB32_Premultiplied);
    before.fill(Qt::transparent);
    QImage after = before;
    QPainter painter(&after);
    const QRect changed(1010, 1010, 28, 28);
    painter.fillRect(changed, QColor(255, 255, 255, 255));
    painter.end();

    const RasterTileDeltaSet delta = buildRasterTileDeltaSet(before, after, changed, 256);
    QCOMPARE(delta.tiles.size(), 4);
    for (const RasterTileDelta &tile : delta.tiles) {
        QVERIFY(tile.rect.intersects(changed));
    }
}


void CoreTests::rasterTileDeltaHandlesPartialEdgeTile()
{
    QImage before(700, 530, QImage::Format_ARGB32_Premultiplied);
    before.fill(QColor(15, 25, 35, 255));
    QImage after = before;
    after.setPixelColor(699, 529, QColor(250, 180, 40, 128));

    const RasterTileDeltaSet delta = buildRasterTileDeltaSet(before,
                                                              after,
                                                              QRect(699, 529, 1, 1),
                                                              256);
    QCOMPARE(delta.tiles.size(), 1);
    QCOMPARE(delta.tiles.constFirst().rect, QRect(512, 512, 188, 18));

    bool ok = false;
    const QImage undone = applyRasterTileDeltaSet(after, delta, false, &ok);
    QVERIFY(ok);
    QCOMPARE(undone, before);
    const QImage redone = applyRasterTileDeltaSet(undone, delta, true, &ok);
    QVERIFY(ok);
    QCOMPARE(redone, after);
}

void CoreTests::rasterTileDeltaRestoresNullRaster()
{
    QImage after(512, 512, QImage::Format_ARGB32_Premultiplied);
    after.fill(Qt::transparent);
    after.setPixelColor(255, 255, QColor(20, 180, 240, 200));

    const RasterTileDeltaSet delta = buildRasterTileDeltaSet({},
                                                              after,
                                                              QRect(255, 255, 1, 1),
                                                              256);
    QVERIFY(!delta.isEmpty());
    QVERIFY(delta.beforeWasNull);

    bool ok = false;
    const QImage before = applyRasterTileDeltaSet(after, delta, false, &ok);
    QVERIFY(ok);
    QVERIFY(before.isNull());

    const QImage redone = applyRasterTileDeltaSet(before, delta, true, &ok);
    QVERIFY(ok);
    QCOMPARE(redone, after);
}


void CoreTests::rasterTileDeltaRejectsUnexpectedState()
{
    QImage before(512, 512, QImage::Format_ARGB32_Premultiplied);
    before.fill(Qt::transparent);
    QImage after = before;
    after.setPixelColor(64, 64, QColor(220, 40, 90, 255));

    const RasterTileDeltaSet delta = buildRasterTileDeltaSet(before,
                                                              after,
                                                              QRect(64, 64, 1, 1),
                                                              256);
    QVERIFY(!delta.isEmpty());

    QImage unexpected = after;
    unexpected.setPixelColor(80, 80, QColor(10, 200, 40, 255));
    bool ok = true;
    const QImage rejected = applyRasterTileDeltaSet(unexpected, delta, false, &ok);
    QVERIFY(!ok);
    QVERIFY(rejected.isNull());
    QCOMPARE(unexpected.pixelColor(64, 64), QColor(220, 40, 90, 255));
    QCOMPARE(unexpected.pixelColor(80, 80), QColor(10, 200, 40, 255));
}


void CoreTests::rasterTileDeltaRejectsCorruptBoundsAndPayload()
{
    QImage before(64, 64, QImage::Format_RGBA8888);
    before.fill(Qt::transparent);
    QImage after = before;
    after.setPixelColor(12, 12, QColor(210, 30, 90, 255));

    RasterTileDeltaSet valid = buildRasterTileDeltaSet(
        before, after, QRect(12, 12, 1, 1), 32);
    QVERIFY(!valid.isEmpty());

    RasterTileDeltaSet corruptBounds = valid;
    corruptBounds.tiles[0].rect.translate(64, 0);
    bool ok = true;
    const QImage rejectedBounds = applyRasterTileDeltaSet(
        after, corruptBounds, false, &ok);
    QVERIFY(!ok);
    QVERIFY(rejectedBounds.isNull());

    RasterTileDeltaSet corruptPayload = valid;
    corruptPayload.tiles[0].compressed = false;
    corruptPayload.tiles[0].payload.chop(1);
    const QImage rejectedPayload = applyRasterTileDeltaSet(
        after, corruptPayload, false, &ok);
    QVERIFY(!ok);
    QVERIFY(rejectedPayload.isNull());

    ChannelTileDeltaSet channel = buildChannelTileDeltaSet(
        before, after, QRect(12, 12, 1, 1), 0, 32);
    QVERIFY(!channel.isEmpty());
    channel.tiles[0].rect = QRect(-1, 0, 32, 32);
    const QImage rejectedChannel = applyChannelTileDeltaSet(
        after, channel, false, &ok);
    QVERIFY(!ok);
    QVERIFY(rejectedChannel.isNull());
}

void CoreTests::maskTileDeltaRestoresCompactMask()
{
    const QSize size(700, 530);
    QImage before(1, 1, QImage::Format_Grayscale8);
    before.fill(255);
    QImage after(size, QImage::Format_Grayscale8);
    after.fill(255);
    for (int y = 180; y < 300; ++y) {
        uchar *row = after.scanLine(y);
        std::fill(row + 245, row + 435, static_cast<uchar>(35));
    }

    const MaskTileDeltaSet delta = buildMaskTileDeltaSet(before,
                                                          after,
                                                          QRect(245, 180, 190, 120),
                                                          size,
                                                          256);
    QVERIFY(!delta.isEmpty());
    QVERIFY(delta.beforeWasCompact);
    QCOMPARE(delta.beforeCompactValue, quint8(255));

    bool ok = false;
    const QImage undone = applyMaskTileDeltaSet(after, delta, false, &ok);
    QVERIFY(ok);
    QCOMPARE(undone.size(), QSize(1, 1));
    QCOMPARE(qGray(undone.pixel(0, 0)), 255);

    const QImage redone = applyMaskTileDeltaSet(undone, delta, true, &ok);
    QVERIFY(ok);
    QCOMPARE(redone, after);
}

void CoreTests::maskTileDeltaUsesOneByteStorage()
{
    const QSize size(2048, 2048);
    QImage before(1, 1, QImage::Format_Grayscale8);
    before.fill(255);
    QImage after(size, QImage::Format_Grayscale8);
    after.fill(255);
    after.scanLine(900)[900] = 0;

    const MaskTileDeltaSet maskDelta = buildMaskTileDeltaSet(before,
                                                              after,
                                                              QRect(900, 900, 1, 1),
                                                              size,
                                                              256);
    QCOMPARE(maskDelta.tiles.size(), 1);
    QCOMPARE(maskDelta.tiles.constFirst().rawByteCount, 256 * 256);
    QVERIFY(maskDelta.storedBytes() < qsizetype(4096));

    QImage rasterBefore(size, QImage::Format_ARGB32_Premultiplied);
    rasterBefore.fill(Qt::transparent);
    QImage rasterAfter = rasterBefore;
    rasterAfter.setPixelColor(900, 900, QColor(255, 255, 255, 255));
    const RasterTileDeltaSet rasterDelta = buildRasterTileDeltaSet(rasterBefore,
                                                                    rasterAfter,
                                                                    QRect(900, 900, 1, 1),
                                                                    256);
    QCOMPARE(rasterDelta.tiles.constFirst().rawByteCount, 256 * 256 * 4);
}

void CoreTests::tiledMaskStrokePaintsAndRestoresCoverage()
{
    const QSize size(600, 320);
    QImage compactWhite(1, 1, QImage::Format_Grayscale8);
    compactWhite.fill(255);
    const QVector<QLineF> segments {
        QLineF(QPointF(-30.0, 128.0), QPointF(300.0, 128.0)),
        QLineF(QPointF(300.0, 128.0), QPointF(630.0, 128.0))
    };

    TiledCanvasEngine engine;
    const QUuid layerId = QUuid::createUuid();
    const auto painted = engine.stampMaskStroke(compactWhite,
                                                 size,
                                                 layerId,
                                                 1,
                                                 segments,
                                                 QTransform(),
                                                 48.0,
                                                 1.0,
                                                 0.8,
                                                 0,
                                                 false,
                                                 false);
    QVERIFY(!painted.image.isNull());
    QCOMPARE(painted.image.format(), QImage::Format_Grayscale8);
    QCOMPARE(painted.image.size(), size);
    QVERIFY(painted.affectedRect.contains(QPoint(255, 128)));
    QVERIFY(painted.affectedRect.contains(QPoint(256, 128)));
    QVERIFY(painted.image.constScanLine(128)[128] < 10);
    QVERIFY(painted.image.constScanLine(128)[255] < 10);
    QVERIFY(painted.image.constScanLine(128)[256] < 10);
    QVERIFY(painted.image.constScanLine(128)[470] < 10);

    const auto restored = engine.stampMaskStroke(painted.image,
                                                  size,
                                                  layerId,
                                                  2,
                                                  segments,
                                                  QTransform(),
                                                  48.0,
                                                  1.0,
                                                  0.8,
                                                  0,
                                                  true,
                                                  false);
    QVERIFY(!restored.image.isNull());
    QVERIFY(restored.image.constScanLine(128)[128] > 245);
    QVERIFY(restored.image.constScanLine(128)[255] > 245);
    QVERIFY(restored.image.constScanLine(128)[256] > 245);
    QVERIFY(restored.image.constScanLine(128)[470] > 245);
}

void CoreTests::nativeGpuMaskStrokeMatchesCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuBrushParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(QStringLiteral("GPU parity passed for mask-stroke test"), true);

    const QSize size(540, 300);
    QImage compactWhite(1, 1, QImage::Format_Grayscale8);
    compactWhite.fill(255);
    const QVector<QLineF> segments {
        QLineF(QPointF(220.0, 32.0), QPointF(320.0, 142.0)),
        QLineF(QPointF(320.0, 142.0), QPointF(430.0, 260.0))
    };
    const QUuid cpuId = QUuid::createUuid();
    const QUuid gpuId = QUuid::createUuid();

    TiledCanvasEngine cpuEngine;
    const auto expected = cpuEngine.stampMaskStroke(compactWhite,
                                                     size,
                                                     cpuId,
                                                     1,
                                                     segments,
                                                     QTransform(),
                                                     37.0,
                                                     0.72,
                                                     0.57,
                                                     64,
                                                     false,
                                                     false);
    const auto actual = backend.stampMaskStroke(compactWhite,
                                                 size,
                                                 gpuId,
                                                 1,
                                                 segments,
                                                 QTransform(),
                                                 37.0,
                                                 0.72,
                                                 0.57,
                                                 64,
                                                 false,
                                                 true);
    QVERIFY(!expected.image.isNull());
    QVERIFY(!actual.image.isNull());
    QVERIFY(actual.usedGpu);
    QCOMPARE(actual.image.size(), expected.image.size());
    int maximumDifference = 0;
    for (int y = 0; y < actual.image.height(); ++y) {
        const uchar *gpu = actual.image.constScanLine(y);
        const uchar *cpu = expected.image.constScanLine(y);
        for (int x = 0; x < actual.image.width(); ++x) {
            maximumDifference = std::max(maximumDifference,
                                         std::abs(static_cast<int>(gpu[x])
                                                  - static_cast<int>(cpu[x])));
        }
    }
    QVERIFY(maximumDifference <= 2);
}


void CoreTests::nativeGpuSelectionAwareRasterStrokeMatchesCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuBrushParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(
        QStringLiteral("GPU parity passed for selection-aware raster-stroke test"), true);

    const QSize size(520, 300);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4] = static_cast<uchar>(15 + x % 100);
            row[x * 4 + 1] = static_cast<uchar>(30 + y % 120);
            row[x * 4 + 2] = 170;
            row[x * 4 + 3] = static_cast<uchar>((x + y) % 7 == 0 ? 0 : 190);
        }
    }
    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.combineShape(QRectF(100.25, 45.25, 310.5, 205.5),
                                      SelectionShape::Ellipse,
                                      SelectionCombineMode::Replace,
                                      true));
    const auto snapshot = selection.snapshot();
    const QVector<QLineF> segments {
        QLineF(QPointF(60.0, 65.0), QPointF(250.0, 150.0)),
        QLineF(QPointF(250.0, 150.0), QPointF(465.0, 245.0))
    };

    TiledCanvasEngine cpuEngine;
    const auto expected = cpuEngine.stampRasterStroke(
        source, size, source.colorSpace(), QUuid::createUuid(), 1,
        segments, QTransform(), 43.0, 0.67, 0.58,
        QColor(230, 45, 90, 210), false, false, QUuid(), &snapshot, QTransform());
    const auto actual = backend.stampRasterStroke(
        source, size, source.colorSpace(), QUuid::createUuid(), 1,
        segments, QTransform(), 43.0, 0.67, 0.58,
        QColor(230, 45, 90, 210), false, true, &snapshot, QTransform());
    QVERIFY2(!expected.image.isNull(), qPrintable(expected.error));
    QVERIFY2(!actual.image.isNull(), qPrintable(actual.error));
    QVERIFY(actual.usedGpu);
    QVERIFY(actual.selectionApplied);
    const QImage cpu = expected.image.convertToFormat(QImage::Format_RGBA8888);
    const QImage gpu = actual.image.convertToFormat(QImage::Format_RGBA8888);
    int maximumDifference = 0;
    for (int y = 0; y < size.height(); ++y) {
        const uchar *cpuRow = cpu.constScanLine(y);
        const uchar *gpuRow = gpu.constScanLine(y);
        for (int x = 0; x < size.width() * 4; ++x) {
            maximumDifference = std::max(maximumDifference,
                                         std::abs(static_cast<int>(cpuRow[x])
                                                  - static_cast<int>(gpuRow[x])));
        }
    }
    QVERIFY2(maximumDifference <= 3,
             qPrintable(QStringLiteral("Maximum CPU/GPU difference was %1")
                            .arg(maximumDifference)));
}

void CoreTests::nativeGpuSelectionAwareMaskStrokeMatchesCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuBrushParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(
        QStringLiteral("GPU parity passed for selection-aware mask-stroke test"), true);

    const QSize size(530, 310);
    QImage source(1, 1, QImage::Format_Grayscale8);
    source.fill(230);
    SelectionMask selection(size);
    selection.selectNone();
    QVERIFY(selection.combineShape(QRectF(72.4, 38.2, 370.7, 225.5),
                                      SelectionShape::Ellipse,
                                      SelectionCombineMode::Replace,
                                      true));
    const auto snapshot = selection.snapshot();
    const QVector<QLineF> segments {
        QLineF(QPointF(40.0, 90.0), QPointF(260.0, 160.0)),
        QLineF(QPointF(260.0, 160.0), QPointF(490.0, 240.0))
    };
    TiledCanvasEngine cpuEngine;
    const auto expected = cpuEngine.stampMaskStroke(
        source, size, QUuid::createUuid(), 1, segments, QTransform(),
        39.0, 0.74, 0.61, 25, false, false, QUuid(), &snapshot, QTransform());
    const auto actual = backend.stampMaskStroke(
        source, size, QUuid::createUuid(), 1, segments, QTransform(),
        39.0, 0.74, 0.61, 25, false, true, &snapshot, QTransform());
    QVERIFY2(!expected.image.isNull(), qPrintable(expected.error));
    QVERIFY2(!actual.image.isNull(), qPrintable(actual.error));
    QVERIFY(actual.usedGpu);
    QVERIFY(actual.selectionApplied);
    int maximumDifference = 0;
    for (int y = 0; y < size.height(); ++y) {
        const uchar *cpu = expected.image.constScanLine(y);
        const uchar *gpu = actual.image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            maximumDifference = std::max(maximumDifference,
                                         std::abs(static_cast<int>(cpu[x])
                                                  - static_cast<int>(gpu[x])));
        }
    }
    QVERIFY(maximumDifference <= 3);
}


void CoreTests::cloneStampCopiesStraightRgbaAndHiddenRgb()
{
    QImage source(QSize(8, 8), QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    uchar *sourcePixel = source.scanLine(2) + 2 * 4;
    sourcePixel[0] = 17;
    sourcePixel[1] = 121;
    sourcePixel[2] = 233;
    sourcePixel[3] = 0;

    QImage destination(QSize(8, 8), QImage::Format_RGBA8888);
    destination.fill(QColor(90, 80, 70, 255));

    CloneStampRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {QLineF(QPointF(5.5, 5.5), QPointF(5.5, 5.5))};
    request.sourceOffsetDocument = QPointF(-3.0, -3.0);
    request.diameterPixels = 4.0;
    request.opacity = 1.0;
    request.hardness = 1.0;

    const CloneStampResult result = applyCloneStamp(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    const uchar *pixel = result.image.constScanLine(5) + 5 * 4;
    QCOMPARE(pixel[0], static_cast<uchar>(17));
    QCOMPARE(pixel[1], static_cast<uchar>(121));
    QCOMPARE(pixel[2], static_cast<uchar>(233));
    QCOMPARE(pixel[3], static_cast<uchar>(0));
    QVERIFY(result.affectedRect.contains(5, 5));
}

void CoreTests::cloneStampSoftTransparencyAvoidsColourHalos()
{
    QImage source(QSize(9, 9), QImage::Format_RGBA8888);
    source.fill(QColor(204, 111, 37, 255));
    QImage destination(QSize(9, 9), QImage::Format_RGBA8888);
    destination.fill(Qt::transparent);

    CloneStampRequest softEdge;
    softEdge.destination = destination;
    softEdge.source = source;
    softEdge.targetSegments = {QLineF(QPointF(4.5, 4.5), QPointF(4.5, 4.5))};
    softEdge.diameterPixels = 6.0;
    softEdge.opacity = 1.0;
    softEdge.hardness = 0.0;
    const CloneStampResult softResult = applyCloneStamp(softEdge);
    QVERIFY2(!softResult.image.isNull(), qPrintable(softResult.error));
    const uchar *softPixel = softResult.image.constScanLine(4) + 6 * 4;
    QVERIFY(softPixel[3] > 0 && softPixel[3] < 255);
    QVERIFY(std::abs(static_cast<int>(softPixel[0]) - 204) <= 1);
    QVERIFY(std::abs(static_cast<int>(softPixel[1]) - 111) <= 1);
    QVERIFY(std::abs(static_cast<int>(softPixel[2]) - 37) <= 1);

    QImage filteredSource(QSize(2, 1), QImage::Format_RGBA8888);
    filteredSource.fill(Qt::transparent);
    uchar *opaque = filteredSource.scanLine(0);
    opaque[0] = 220;
    opaque[1] = 80;
    opaque[2] = 35;
    opaque[3] = 255;
    uchar *transparent = filteredSource.scanLine(0) + 4;
    transparent[0] = 0;
    transparent[1] = 0;
    transparent[2] = 0;
    transparent[3] = 0;

    QImage onePixel(QSize(1, 1), QImage::Format_RGBA8888);
    onePixel.fill(Qt::transparent);
    CloneStampRequest filtered;
    filtered.destination = onePixel;
    filtered.source = filteredSource;
    filtered.targetSegments = {QLineF(QPointF(0.5, 0.5), QPointF(0.5, 0.5))};
    filtered.sourceOffsetDocument = QPointF(0.5, 0.0);
    filtered.diameterPixels = 2.0;
    filtered.opacity = 1.0;
    filtered.hardness = 1.0;
    const CloneStampResult filteredResult = applyCloneStamp(filtered);
    QVERIFY2(!filteredResult.image.isNull(), qPrintable(filteredResult.error));
    const uchar *filteredPixel = filteredResult.image.constScanLine(0);
    QCOMPARE(filteredPixel[3], static_cast<uchar>(128));
    QVERIFY(std::abs(static_cast<int>(filteredPixel[0]) - 220) <= 1);
    QVERIFY(std::abs(static_cast<int>(filteredPixel[1]) - 80) <= 1);
    QVERIFY(std::abs(static_cast<int>(filteredPixel[2]) - 35) <= 1);
}

void CoreTests::cloneStampLowOpacityOverlapAvoidsChromaticContours()
{
    const QSize size(220, 100);
    const QColor destinationColour(20, 100, 200, 255);
    const QColor sourceColour(220, 40, 30, 255);
    QImage destination(size, QImage::Format_RGBA8888);
    destination.fill(destinationColour);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(sourceColour);

    CloneStampRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {
        QLineF(QPointF(20.5, 50.5), QPointF(180.5, 50.5))
    };
    request.diameterPixels = 40.0;
    request.opacity = 0.05;
    request.hardness = 0.0;

    const CloneStampResult result = applyCloneStamp(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    QVERIFY(!result.affectedRect.isEmpty());

    const double destinationChannels[3] {
        destinationColour.redF(), destinationColour.greenF(), destinationColour.blueF()
    };
    const double sourceChannels[3] {
        sourceColour.redF(), sourceColour.greenF(), sourceColour.blueF()
    };
    double maximumCoverageSpread = 0.0;
    int changedPixels = 0;
    for (int y = result.affectedRect.top(); y <= result.affectedRect.bottom(); ++y) {
        const uchar *row = result.image.constScanLine(y);
        for (int x = result.affectedRect.left(); x <= result.affectedRect.right(); ++x) {
            const uchar *pixel = row + x * 4;
            if (pixel[0] == destinationColour.red()
                && pixel[1] == destinationColour.green()
                && pixel[2] == destinationColour.blue()) {
                continue;
            }
            ++changedPixels;
            double inferredCoverage[3] {};
            for (int component = 0; component < 3; ++component) {
                const double output = pixel[component] / 255.0;
                inferredCoverage[component] = (output - destinationChannels[component])
                    / (sourceChannels[component] - destinationChannels[component]);
            }
            const auto range = std::minmax({inferredCoverage[0],
                                            inferredCoverage[1],
                                            inferredCoverage[2]});
            maximumCoverageSpread = std::max(maximumCoverageSpread,
                                              range.second - range.first);
        }
    }
    QVERIFY(changedPixels > 0);
    QVERIFY2(maximumCoverageSpread <= 0.015,
             qPrintable(QStringLiteral(
                 "Low-opacity Clone Stamp RGB coverage diverged by %1")
                            .arg(maximumCoverageSpread, 0, 'f', 6)));
}

void CoreTests::cloneStampTargetsChannelsAndMasks()
{
    QImage source(QSize(8, 8), QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    uchar *sourcePixel = source.scanLine(1) + 1 * 4;
    sourcePixel[0] = 20;
    sourcePixel[1] = 60;
    sourcePixel[2] = 220;
    sourcePixel[3] = 77;

    QImage destination(QSize(8, 8), QImage::Format_RGBA8888);
    destination.fill(QColor(100, 110, 120, 130));

    CloneStampRequest component;
    component.destination = destination;
    component.source = source;
    component.targetSegments = {QLineF(QPointF(4.5, 4.5), QPointF(4.5, 4.5))};
    component.sourceOffsetDocument = QPointF(-3.0, -3.0);
    component.diameterPixels = 4.0;
    component.target = CloneStampTarget::ComponentChannel;
    component.sample = CloneStampSample::Component;
    component.componentIndex = 1;
    const CloneStampResult componentResult = applyCloneStamp(component);
    QVERIFY2(!componentResult.image.isNull(), qPrintable(componentResult.error));
    const uchar *componentPixel = componentResult.image.constScanLine(4) + 4 * 4;
    QCOMPARE(componentPixel[0], static_cast<uchar>(100));
    QCOMPARE(componentPixel[1], static_cast<uchar>(60));
    QCOMPARE(componentPixel[2], static_cast<uchar>(120));
    QCOMPARE(componentPixel[3], static_cast<uchar>(130));

    CloneStampRequest alpha = component;
    alpha.target = CloneStampTarget::ComponentChannel;
    alpha.sample = CloneStampSample::Alpha;
    alpha.componentIndex = 3;
    const CloneStampResult alphaResult = applyCloneStamp(alpha);
    QVERIFY2(!alphaResult.image.isNull(), qPrintable(alphaResult.error));
    const uchar *alphaPixel = alphaResult.image.constScanLine(4) + 4 * 4;
    QCOMPARE(alphaPixel[0], static_cast<uchar>(100));
    QCOMPARE(alphaPixel[1], static_cast<uchar>(110));
    QCOMPARE(alphaPixel[2], static_cast<uchar>(120));
    QCOMPARE(alphaPixel[3], static_cast<uchar>(77));

    QImage maskSource(QSize(8, 8), QImage::Format_Grayscale8);
    maskSource.fill(0);
    maskSource.scanLine(1)[1] = 187;
    QImage maskDestination(QSize(8, 8), QImage::Format_Grayscale8);
    maskDestination.fill(42);
    CloneStampRequest mask;
    mask.destination = maskDestination;
    mask.source = maskSource;
    mask.targetSegments = component.targetSegments;
    mask.sourceOffsetDocument = component.sourceOffsetDocument;
    mask.diameterPixels = 4.0;
    mask.target = CloneStampTarget::Mask;
    mask.sample = CloneStampSample::Luminance;
    const CloneStampResult maskResult = applyCloneStamp(mask);
    QVERIFY2(!maskResult.image.isNull(), qPrintable(maskResult.error));
    QCOMPARE(maskResult.image.constScanLine(4)[4], static_cast<uchar>(187));
}

void CoreTests::cloneStampHonoursTransformsAndOutsideSource()
{
    QImage source(QSize(8, 8), QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    uchar *sourcePixel = source.scanLine(1) + 1 * 4;
    sourcePixel[0] = 211;
    sourcePixel[1] = 73;
    sourcePixel[2] = 19;
    sourcePixel[3] = 255;
    QImage destination(QSize(8, 8), QImage::Format_RGBA8888);
    destination.fill(QColor(4, 5, 6, 255));

    CloneStampRequest transformed;
    transformed.destination = destination;
    transformed.source = source;
    transformed.targetSegments = {QLineF(QPointF(1.5, 1.5), QPointF(1.5, 1.5))};
    transformed.targetLayerToDocument = QTransform::fromTranslate(10.0, 20.0);
    transformed.sourceOffsetDocument = QPointF(-10.0, -20.0);
    transformed.diameterPixels = 4.0;
    const CloneStampResult transformedResult = applyCloneStamp(transformed);
    QVERIFY2(!transformedResult.image.isNull(), qPrintable(transformedResult.error));
    const uchar *pixel = transformedResult.image.constScanLine(1) + 1 * 4;
    QCOMPARE(pixel[0], static_cast<uchar>(211));
    QCOMPARE(pixel[1], static_cast<uchar>(73));
    QCOMPARE(pixel[2], static_cast<uchar>(19));

    CloneStampRequest outside = transformed;
    outside.sourceOffsetDocument = QPointF(1000.0, 1000.0);
    const CloneStampResult outsideResult = applyCloneStamp(outside);
    QVERIFY2(!outsideResult.image.isNull(), qPrintable(outsideResult.error));
    QVERIFY(exactImagesEqual(outsideResult.image, destination));
}

void CoreTests::cloneStampPreservesSixteenBitComponents()
{
    QImage source(QSize(6, 6), QImage::Format_RGBA64);
    source.fill(Qt::transparent);
    auto *sourceRow = reinterpret_cast<QRgba64 *>(source.scanLine(1));
    sourceRow[1] = QRgba64::fromRgba64(1234, 23456, 61234, 0);
    QImage destination(QSize(6, 6), QImage::Format_RGBA64);
    destination.fill(QColor(20, 30, 40, 255));

    CloneStampRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {QLineF(QPointF(3.5, 3.5), QPointF(3.5, 3.5))};
    request.sourceOffsetDocument = QPointF(-2.0, -2.0);
    request.diameterPixels = 4.0;
    const CloneStampResult result = applyCloneStamp(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    const auto *row = reinterpret_cast<const QRgba64 *>(result.image.constScanLine(3));
    QCOMPARE(row[3].red(), static_cast<quint16>(1234));
    QCOMPARE(row[3].green(), static_cast<quint16>(23456));
    QCOMPARE(row[3].blue(), static_cast<quint16>(61234));
    QCOMPARE(row[3].alpha(), static_cast<quint16>(0));
}

void CoreTests::cloneStampTiledCpuFallbackMatchesReference()
{
    const QSize size(300, 270);
    QImage destination(size, QImage::Format_RGBA8888);
    QImage source(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *destinationRow = destination.scanLine(y);
        uchar *sourceRow = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int offset = x * 4;
            destinationRow[offset] = static_cast<uchar>((19 + x * 3 + y) & 255);
            destinationRow[offset + 1] = static_cast<uchar>((71 + x + y * 5) & 255);
            destinationRow[offset + 2] = static_cast<uchar>((137 + x * 7 + y * 2) & 255);
            destinationRow[offset + 3] = static_cast<uchar>(
                ((x + y) % 17 == 0) ? 0 : 37 + ((x * 5 + y * 3) % 219));
            sourceRow[offset] = static_cast<uchar>((233 + x * 5 + y * 7) & 255);
            sourceRow[offset + 1] = static_cast<uchar>((29 + x * 11 + y) & 255);
            sourceRow[offset + 2] = static_cast<uchar>((101 + x * 2 + y * 13) & 255);
            sourceRow[offset + 3] = static_cast<uchar>(
                ((x * 2 + y) % 19 == 0) ? 0 : 23 + ((x * 7 + y * 9) % 233));
        }
    }

    CloneStampRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {
        QLineF(QPointF(225.25, 238.5), QPointF(284.75, 262.25))
    };
    request.targetLayerToDocument = QTransform::fromTranslate(3.125, -2.75);
    request.sourceDocumentToLayer = QTransform::fromTranslate(-1.25, 0.625);
    request.sourceOffsetDocument = QPointF(-119.375, -112.125);
    request.diameterPixels = 41.0;
    request.opacity = 0.69;
    request.hardness = 0.42;

    const CloneStampResult expected = applyCloneStamp(request);
    TiledCanvasEngine engine;
    const auto actual = engine.stampCloneStroke(request,
                                                size,
                                                QUuid::createUuid(),
                                                1,
                                                false);
    QVERIFY2(!expected.image.isNull(), qPrintable(expected.error));
    QVERIFY2(!actual.image.isNull(), qPrintable(actual.error));
    QVERIFY(!actual.usedGpu);
    QVERIFY(!actual.selectionApplied);
    QCOMPARE(actual.affectedRect, expected.affectedRect);
    QVERIFY(exactImagesEqual(actual.image, expected.image));
}

void CoreTests::nativeGpuCloneStampMatchesCpuWhenAvailable()
{
    RenderBackend &backend = RenderBackend::instance();
    if (!backend.initialiseGpuFoundation()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    if (!backend.webGpuCloneStampParityPassed()) {
        QSKIP(qPrintable(backend.statusText()));
    }
    backend.setExternalDiagnosticStatus(
        QStringLiteral("GPU parity passed for Clone Stamp test"), true);

    const QSize size(520, 310);
    QImage destination(size, QImage::Format_RGBA8888);
    QImage source(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *destinationRow = destination.scanLine(y);
        uchar *sourceRow = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int offset = x * 4;
            destinationRow[offset] = static_cast<uchar>((11 + x * 3 + y * 7) & 255);
            destinationRow[offset + 1] = static_cast<uchar>((89 + x * 5 + y) & 255);
            destinationRow[offset + 2] = static_cast<uchar>((173 + x + y * 11) & 255);
            destinationRow[offset + 3] = static_cast<uchar>(
                ((x + y * 2) % 31 == 0) ? 0 : 21 + ((x * 7 + y * 5) % 235));
            sourceRow[offset] = static_cast<uchar>((241 + x * 7 + y * 3) & 255);
            sourceRow[offset + 1] = static_cast<uchar>((37 + x * 2 + y * 9) & 255);
            sourceRow[offset + 2] = static_cast<uchar>((149 + x * 11 + y) & 255);
            sourceRow[offset + 3] = static_cast<uchar>(
                ((x * 3 + y) % 23 == 0) ? 0 : 17 + ((x * 5 + y * 13) % 239));
        }
    }

    CloneStampRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {
        QLineF(QPointF(214.5, 72.25), QPointF(305.75, 154.5)),
        QLineF(QPointF(305.75, 154.5), QPointF(431.25, 267.75))
    };
    request.targetLayerToDocument = QTransform::fromTranslate(4.25, -3.5);
    request.sourceDocumentToLayer = QTransform::fromTranslate(-1.125, 2.375);
    request.sourceOffsetDocument = QPointF(-91.625, -38.875);
    request.diameterPixels = 47.0;
    request.opacity = 0.73;
    request.hardness = 0.49;

    const CloneStampResult expected = applyCloneStamp(request);
    const auto actual = backend.stampCloneStroke(request,
                                                 size,
                                                 QUuid::createUuid(),
                                                 1,
                                                 true);
    QVERIFY2(!expected.image.isNull(), qPrintable(expected.error));
    QVERIFY2(!actual.image.isNull(), qPrintable(actual.error));
    QVERIFY(actual.usedGpu);
    QVERIFY(actual.selectionApplied);
    QCOMPARE(actual.affectedRect, expected.affectedRect);

    const QImage cpu = expected.image.convertToFormat(QImage::Format_RGBA8888);
    const QImage gpu = actual.image.convertToFormat(QImage::Format_RGBA8888);
    int maximumDifference = 0;
    for (int y = 0; y < size.height(); ++y) {
        const uchar *cpuRow = cpu.constScanLine(y);
        const uchar *gpuRow = gpu.constScanLine(y);
        for (int x = 0; x < size.width() * 4; ++x) {
            maximumDifference = std::max(maximumDifference,
                                         std::abs(static_cast<int>(cpuRow[x])
                                                  - static_cast<int>(gpuRow[x])));
        }
    }
    // The CPU reference evaluates transformed bilinear sampling and complete
    // stroke coverage in double precision; WGSL uses f32 before the single
    // UNORM8 write. A bounded three-code-value delta is the startup contract
    // for Clone Stamp only, not for hierarchy or mask compositing.
    QVERIFY2(maximumDifference <= 3,
             qPrintable(QStringLiteral("Maximum Clone Stamp CPU/GPU difference was %1")
                            .arg(maximumDifference)));
}

void CoreTests::healingBrushTransfersDetailAndAdaptsColour()
{
    const QSize size(33, 33);
    QImage source(size, QImage::Format_RGBA8888);
    QImage destination(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *sourceRow = source.scanLine(y);
        uchar *destinationRow = destination.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int detail = ((x + y) & 1) ? 34 : -34;
            uchar *s = sourceRow + x * 4;
            s[0] = static_cast<uchar>(std::clamp(190 + detail, 0, 255));
            s[1] = static_cast<uchar>(std::clamp(82 + detail, 0, 255));
            s[2] = static_cast<uchar>(std::clamp(58 + detail, 0, 255));
            s[3] = 255;
            uchar *d = destinationRow + x * 4;
            d[0] = 46;
            d[1] = 138;
            d[2] = 194;
            d[3] = 255;
        }
    }

    HealingBrushRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {QLineF(QPointF(16.5, 16.5), QPointF(16.5, 16.5))};
    request.diameterPixels = 17.0;
    request.opacity = 1.0;
    request.hardness = 1.0;
    const HealingBrushResult result = applyHealingBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    const uchar *pixel = result.image.constScanLine(16) + 16 * 4;
    QVERIFY(pixel[2] > pixel[0]);
    QVERIFY(std::abs(static_cast<int>(pixel[0]) - 46) > 4
            || std::abs(static_cast<int>(pixel[1]) - 138) > 4
            || std::abs(static_cast<int>(pixel[2]) - 194) > 4);
    QVERIFY(pixel[0] < 120); // source's warm low-frequency colour was not copied
    QCOMPARE(pixel[3], static_cast<uchar>(255));
}

void CoreTests::healingBrushSeamlesslyRemovesDestinationStep()
{
    const QSize size(49, 49);
    QImage source(size, QImage::Format_RGBA8888);
    QImage destination(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *sourceRow = source.scanLine(y);
        uchar *destinationRow = destination.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *s = sourceRow + x * 4;
            const int texture = ((x * 7 + y * 11) % 9) - 4;
            s[0] = static_cast<uchar>(128 + texture);
            s[1] = static_cast<uchar>(132 + texture);
            s[2] = static_cast<uchar>(126 + texture);
            s[3] = 255;

            uchar *d = destinationRow + x * 4;
            const int value = x < size.width() / 2 ? 34 : 222;
            d[0] = static_cast<uchar>(value);
            d[1] = static_cast<uchar>(value);
            d[2] = static_cast<uchar>(value);
            d[3] = 255;
        }
    }

    HealingBrushRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {QLineF(QPointF(24.5, 24.5), QPointF(24.5, 24.5))};
    request.diameterPixels = 31.0;
    request.opacity = 1.0;
    request.hardness = 0.75;
    const HealingBrushResult result = applyHealingBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));

    const uchar *row = result.image.constScanLine(24);
    const int beforeJump = 222 - 34;
    const int afterJump = std::abs(static_cast<int>(row[25 * 4])
                                   - static_cast<int>(row[23 * 4]));
    QVERIFY2(afterJump < beforeJump / 3,
             qPrintable(QStringLiteral("Healed step remained too sharp: %1")
                            .arg(afterJump)));

    // The correction must be a smooth boundary-matched transition, not merely
    // a faint source imprint laid over the original discontinuity.
    int largestInteriorStep = 0;
    for (int x = 17; x < 31; ++x) {
        largestInteriorStep = std::max(
            largestInteriorStep,
            std::abs(static_cast<int>(row[(x + 1) * 4])
                     - static_cast<int>(row[x * 4])));
    }
    QVERIFY2(largestInteriorStep < beforeJump / 4,
             qPrintable(QStringLiteral("Healing retained an interior edge of %1")
                            .arg(largestInteriorStep)));
}

void CoreTests::healingBrushPreservesDestinationAlphaAndHiddenRgb()
{
    const QSize size(24, 24);
    QImage source(size, QImage::Format_RGBA8888);
    QImage destination(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *sourceRow = source.scanLine(y);
        uchar *destinationRow = destination.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *s = sourceRow + x * 4;
            s[0] = static_cast<uchar>(60 + ((x * 17 + y * 3) % 120));
            s[1] = static_cast<uchar>(40 + ((x * 5 + y * 19) % 140));
            s[2] = static_cast<uchar>(80 + ((x * 11 + y * 7) % 100));
            s[3] = 255;
            uchar *d = destinationRow + x * 4;
            d[0] = 12;
            d[1] = 22;
            d[2] = 32;
            d[3] = static_cast<uchar>((x + y) % 3 == 0 ? 0
                                      : ((x + y) % 3 == 1 ? 127 : 255));
        }
    }
    const QImage before = destination;

    HealingBrushRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {QLineF(QPointF(5.5, 12.5), QPointF(18.5, 12.5))};
    request.diameterPixels = 13.0;
    request.opacity = 0.8;
    request.hardness = 0.45;
    const HealingBrushResult result = applyHealingBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));

    bool hiddenRgbChanged = false;
    for (int y = 0; y < size.height(); ++y) {
        const uchar *beforeRow = before.constScanLine(y);
        const uchar *afterRow = result.image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const uchar *b = beforeRow + x * 4;
            const uchar *a = afterRow + x * 4;
            QCOMPARE(a[3], b[3]);
            if (b[3] == 0 && (a[0] != b[0] || a[1] != b[1] || a[2] != b[2])) {
                hiddenRgbChanged = true;
            }
        }
    }
    QVERIFY(hiddenRgbChanged);
}

void CoreTests::healingBrushPreservesSixteenBitAlpha()
{
    const QSize size(20, 20);
    QImage source(size, QImage::Format_RGBA64);
    QImage destination(size, QImage::Format_RGBA64);
    for (int y = 0; y < size.height(); ++y) {
        auto *sourceRow = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        auto *destinationRow = reinterpret_cast<QRgba64 *>(destination.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            sourceRow[x] = QRgba64::fromRgba64(
                static_cast<quint16>(18000 + x * 1200),
                static_cast<quint16>(24000 + y * 900),
                static_cast<quint16>(12000 + ((x + y) & 1) * 18000),
                65535);
            destinationRow[x] = QRgba64::fromRgba64(
                9000, 32000, 46000,
                static_cast<quint16>((x * 3191 + y * 1877) & 0xffff));
        }
    }
    const QImage before = destination;

    HealingBrushRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {QLineF(QPointF(10.5, 10.5), QPointF(10.5, 10.5))};
    request.diameterPixels = 11.0;
    request.opacity = 1.0;
    request.hardness = 1.0;
    const HealingBrushResult result = applyHealingBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    const auto *beforeRow = reinterpret_cast<const QRgba64 *>(before.constScanLine(10));
    const auto *afterRow = reinterpret_cast<const QRgba64 *>(result.image.constScanLine(10));
    QCOMPARE(afterRow[10].alpha(), beforeRow[10].alpha());
    QVERIFY(afterRow[10].red() != beforeRow[10].red()
            || afterRow[10].green() != beforeRow[10].green()
            || afterRow[10].blue() != beforeRow[10].blue());
}


void CoreTests::healingBrushOutsideSourceIsNoOp()
{
    QImage source(12, 12, QImage::Format_RGBA8888);
    source.fill(QColor(220, 40, 80, 255));
    QImage destination(20, 20, QImage::Format_RGBA8888);
    destination.fill(QColor(25, 100, 180, 173));
    const QImage before = destination;

    HealingBrushRequest request;
    request.destination = destination;
    request.source = source;
    request.targetSegments = {QLineF(QPointF(10.5, 10.5), QPointF(10.5, 10.5))};
    request.sourceOffsetDocument = QPointF(1000.0, 1000.0);
    request.diameterPixels = 15.0;
    request.opacity = 1.0;
    request.hardness = 0.0;
    const HealingBrushResult result = applyHealingBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    QCOMPARE(result.image, before);
}

void CoreTests::spotHealingChoosesDeterministicNonOverlappingSource()
{
    const QSize size(96, 80);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int texture = ((x * 13 + y * 7) % 11) - 5;
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(std::clamp(118 + x / 3 + texture, 0, 255));
            pixel[1] = static_cast<uchar>(std::clamp(132 + x / 4 + texture, 0, 255));
            pixel[2] = static_cast<uchar>(std::clamp(126 + x / 5 + texture, 0, 255));
            pixel[3] = 255;
        }
    }

    const QPoint centre(48, 40);
    for (int y = centre.y() - 5; y <= centre.y() + 5; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = centre.x() - 5; x <= centre.x() + 5; ++x) {
            const int dx = x - centre.x();
            const int dy = y - centre.y();
            if (dx * dx + dy * dy <= 25) {
                uchar *pixel = row + x * 4;
                pixel[0] = 24;
                pixel[1] = 18;
                pixel[2] = 20;
            }
        }
    }
    const QImage before = image;

    SpotHealingRequest request;
    request.destination = image;
    request.source = image;
    request.targetSegments = {
        QLineF(QPointF(centre.x() + 0.5, centre.y() + 0.5),
               QPointF(centre.x() + 0.5, centre.y() + 0.5))
    };
    request.diameterPixels = 19.0;
    request.opacity = 1.0;
    request.hardness = 0.8;

    const SpotHealingResult first = applySpotHealing(request);
    QVERIFY2(!first.image.isNull(), qPrintable(first.error));
    QVERIFY(first.candidatesEvaluated > 0);
    QVERIFY(QLineF(QPointF(), first.sourceOffsetDocument).length()
            >= request.diameterPixels * 0.9);

    const SpotHealingResult second = applySpotHealing(request);
    QVERIFY2(!second.image.isNull(), qPrintable(second.error));
    QCOMPARE(first.sourceOffsetDocument, second.sourceOffsetDocument);
    QVERIFY(exactImagesEqual(first.image, second.image));

    const uchar *beforeCentre = before.constScanLine(centre.y()) + centre.x() * 4;
    const uchar *afterCentre = first.image.constScanLine(centre.y()) + centre.x() * 4;
    const int expectedRed = 118 + centre.x() / 3;
    QVERIFY(std::abs(static_cast<int>(afterCentre[0]) - expectedRed)
            < std::abs(static_cast<int>(beforeCentre[0]) - expectedRed) / 2);
    QVERIFY(afterCentre[0] > 70);
    QCOMPARE(afterCentre[3], static_cast<uchar>(255));
}

void CoreTests::spotHealingPreservesDestinationAlpha()
{
    const QSize size(64, 64);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(80 + ((x * 7 + y * 3) % 90));
            pixel[1] = static_cast<uchar>(70 + ((x * 5 + y * 11) % 100));
            pixel[2] = static_cast<uchar>(90 + ((x * 13 + y * 2) % 80));
            pixel[3] = static_cast<uchar>((x * 17 + y * 29) & 0xff);
        }
    }
    for (int y = 28; y <= 36; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 28; x <= 36; ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = 240;
            pixel[1] = 20;
            pixel[2] = 30;
        }
    }
    const QImage before = image;

    SpotHealingRequest request;
    request.destination = image;
    request.source = image;
    request.targetSegments = {QLineF(QPointF(32.5, 32.5), QPointF(32.5, 32.5))};
    request.diameterPixels = 17.0;
    request.opacity = 0.85;
    request.hardness = 0.5;
    const SpotHealingResult result = applySpotHealing(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));

    for (int y = 0; y < size.height(); ++y) {
        const uchar *beforeRow = before.constScanLine(y);
        const uchar *afterRow = result.image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(afterRow[x * 4 + 3], beforeRow[x * 4 + 3]);
        }
    }
}


void CoreTests::patchToolSourceModeReplacesSelectedStructure()
{
    const QSize size(72, 48);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int texture = ((x * 7 + y * 11) % 9) - 4;
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(std::clamp(112 + x / 4 + texture, 0, 255));
            pixel[1] = static_cast<uchar>(std::clamp(126 + x / 5 + texture, 0, 255));
            pixel[2] = static_cast<uchar>(std::clamp(118 + x / 6 + texture, 0, 255));
            pixel[3] = 255;
        }
    }
    // A strong unwanted vertical structure in the selected destination.
    for (int y = 14; y < 34; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 48; x < 53; ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = 236;
            pixel[1] = 24;
            pixel[2] = 30;
        }
    }
    const QImage before = image;

    SelectionMask selection(size);
    QImage coverage(QSize(18, 24), QImage::Format_Grayscale8);
    coverage.fill(255);
    QVERIFY(selection.combineCoverageImage(QRect(42, 12, 18, 24),
                                            coverage,
                                            SelectionCombineMode::Replace));

    PatchToolRequest request;
    request.destination = image;
    request.source = image;
    request.selection = selection.snapshot();
    request.sourceOffsetDocument = QPointF(-32.0, 0.0);
    request.opacity = 1.0;
    const PatchToolResult result = applyPatchTool(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    QVERIFY(!result.affectedRect.isEmpty());

    const uchar *beforeCentre = before.constScanLine(24) + 50 * 4;
    const uchar *afterCentre = result.image.constScanLine(24) + 50 * 4;
    const uchar *sourceCentre = before.constScanLine(24) + 18 * 4;
    const int beforeDistance = std::abs(static_cast<int>(beforeCentre[0])
                                        - static_cast<int>(sourceCentre[0]))
        + std::abs(static_cast<int>(beforeCentre[1])
                   - static_cast<int>(sourceCentre[1]))
        + std::abs(static_cast<int>(beforeCentre[2])
                   - static_cast<int>(sourceCentre[2]));
    const int afterDistance = std::abs(static_cast<int>(afterCentre[0])
                                       - static_cast<int>(sourceCentre[0]))
        + std::abs(static_cast<int>(afterCentre[1])
                   - static_cast<int>(sourceCentre[1]))
        + std::abs(static_cast<int>(afterCentre[2])
                   - static_cast<int>(sourceCentre[2]));
    QVERIFY2(afterDistance < beforeDistance / 3,
             qPrintable(QStringLiteral("Patch retained too much destination structure: %1 vs %2")
                            .arg(afterDistance)
                            .arg(beforeDistance)));
    QCOMPARE(afterCentre[3], beforeCentre[3]);
}

void CoreTests::patchToolDestinationModeMovesSourceAndPreservesAlpha()
{
    const QSize size(80, 48);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = 40;
            pixel[1] = 70;
            pixel[2] = 130;
            pixel[3] = static_cast<uchar>((x * 17 + y * 29) & 0xff);
        }
    }
    // Add a strong interior structure while keeping the selection boundary
    // close to the destination background. Poisson cloning should transfer
    // this structure, not the source patch's absolute low-frequency colour.
    for (int y = 21; y < 27; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 14; x < 20; ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = 225;
            pixel[1] = 210;
            pixel[2] = 52;
        }
    }
    const QImage before = image;

    SelectionMask selection(size);
    QImage coverage(QSize(14, 14), QImage::Format_Grayscale8);
    coverage.fill(255);
    QVERIFY(selection.combineCoverageImage(QRect(10, 17, 14, 14),
                                            coverage,
                                            SelectionCombineMode::Replace));

    PatchToolRequest request;
    request.destination = image;
    request.source = image;
    request.selection = selection.snapshot();
    request.destinationOffsetDocument = QPointF(42.0, 0.0);
    request.sourceOffsetDocument = QPointF(-42.0, 0.0);
    request.opacity = 1.0;
    const PatchToolResult result = applyPatchTool(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));

    // The immutable source region is not cleared or modified by destination mode.
    QCOMPARE(result.image.pixelColor(16, 24), before.pixelColor(16, 24));
    const QColor beforeDestination = before.pixelColor(58, 24);
    const QColor afterDestination = result.image.pixelColor(58, 24);
    QVERIFY(afterDestination.red() > beforeDestination.red() + 45);
    QVERIFY(afterDestination.green() > beforeDestination.green() + 45);

    for (int y = 0; y < size.height(); ++y) {
        const uchar *beforeRow = before.constScanLine(y);
        const uchar *afterRow = result.image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(afterRow[x * 4 + 3], beforeRow[x * 4 + 3]);
        }
    }
}


void CoreTests::patchToolPreservesSixteenBitAlpha()
{
    const QSize size(28, 20);
    QImage image(size, QImage::Format_RGBA64);
    for (int y = 0; y < size.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(9000 + x * 900),
                static_cast<quint16>(18000 + y * 1100),
                static_cast<quint16>(12000 + ((x + y) & 1) * 24000),
                static_cast<quint16>((x * 3191 + y * 1877) & 0xffff));
        }
    }
    for (int y = 8; y < 13; ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        for (int x = 5; x < 10; ++x) {
            row[x] = QRgba64::fromRgba64(54000, 12000, 46000, row[x].alpha());
        }
    }
    const QImage before = image;

    SelectionMask selection(size);
    QImage coverage(QSize(9, 10), QImage::Format_Grayscale8);
    coverage.fill(255);
    QVERIFY(selection.combineCoverageImage(QRect(15, 5, 9, 10),
                                            coverage,
                                            SelectionCombineMode::Replace));

    PatchToolRequest request;
    request.destination = image;
    request.source = image;
    request.selection = selection.snapshot();
    request.sourceOffsetDocument = QPointF(-12.0, 0.0);
    request.opacity = 0.8;
    const PatchToolResult result = applyPatchTool(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));

    bool rgbChanged = false;
    for (int y = 0; y < size.height(); ++y) {
        const auto *beforeRow = reinterpret_cast<const QRgba64 *>(before.constScanLine(y));
        const auto *afterRow = reinterpret_cast<const QRgba64 *>(result.image.constScanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(afterRow[x].alpha(), beforeRow[x].alpha());
            if (afterRow[x].red() != beforeRow[x].red()
                || afterRow[x].green() != beforeRow[x].green()
                || afterRow[x].blue() != beforeRow[x].blue()) {
                rgbChanged = true;
            }
        }
    }
    QVERIFY(rgbChanged);
}

void CoreTests::retouchCancellationNeverPublishesPartialResult()
{
    QImage image(32, 32, QImage::Format_RGBA8888);
    image.fill(QColor(80, 120, 170, 255));
    const auto cancelled = std::make_shared<std::atomic_bool>(true);

    HealingBrushRequest healing;
    healing.destination = image;
    healing.source = image;
    healing.targetSegments = {
        QLineF(QPointF(8.5, 16.5), QPointF(23.5, 16.5))
    };
    healing.diameterPixels = 13.0;
    healing.cancelRequested = cancelled;
    const HealingBrushResult healingResult = applyHealingBrush(healing);
    QVERIFY(healingResult.cancelled);
    QVERIFY(healingResult.image.isNull());
    QVERIFY(healingResult.affectedRect.isEmpty());
    QVERIFY(healingResult.error.isEmpty());

    SpotHealingRequest spot;
    spot.destination = image;
    spot.source = image;
    spot.targetSegments = healing.targetSegments;
    spot.diameterPixels = 13.0;
    spot.cancelRequested = cancelled;
    const SpotHealingResult spotResult = applySpotHealing(spot);
    QVERIFY(spotResult.cancelled);
    QVERIFY(spotResult.image.isNull());
    QVERIFY(spotResult.affectedRect.isEmpty());
    QVERIFY(spotResult.error.isEmpty());

    SelectionMask selection(image.size());
    selection.selectAll();
    PatchToolRequest patch;
    patch.destination = image;
    patch.source = image;
    patch.selection = selection.snapshot();
    patch.cancelRequested = cancelled;
    const PatchToolResult patchResult = applyPatchTool(patch);
    QVERIFY(patchResult.cancelled);
    QVERIFY(patchResult.image.isNull());
    QVERIFY(patchResult.affectedRect.isEmpty());
    QVERIFY(patchResult.error.isEmpty());
}

void CoreTests::dodgeBurnRespectRangesAndPreserveRasterAlpha()
{
    const QSize size(90, 30);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int value = x < 30 ? 45 : x < 60 ? 188 : 250;
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(value);
            pixel[1] = static_cast<uchar>(value);
            pixel[2] = static_cast<uchar>(value);
            pixel[3] = static_cast<uchar>((x * 31 + y * 17) & 0xff);
        }
    }

    ToneBrushRequest request;
    request.destination = image;
    request.targetSegments = {
        QLineF(QPointF(0.5, 15.5), QPointF(89.5, 15.5))
    };
    request.diameterPixels = 36.0;
    request.strength = 0.35;
    request.hardness = 1.0;
    request.operation = ToneBrushOperation::Dodge;
    request.range = ToneBrushRange::Midtones;
    request.target = ToneBrushTarget::RasterRgb;
    request.protectTones = true;
    const ToneBrushResult dodged = applyToneBrush(request);
    QVERIFY2(!dodged.image.isNull(), qPrintable(dodged.error));

    const auto changeAt = [&](const QImage &result, const int x) {
        const uchar *before = image.constScanLine(15) + x * 4;
        const uchar *after = result.constScanLine(15) + x * 4;
        return static_cast<int>(after[0]) - static_cast<int>(before[0]);
    };
    const int darkDodge = changeAt(dodged.image, 15);
    const int middleDodge = changeAt(dodged.image, 45);
    const int brightDodge = changeAt(dodged.image, 75);
    QVERIFY(middleDodge > darkDodge + 10);
    QVERIFY(middleDodge > brightDodge + 10);

    request.operation = ToneBrushOperation::Burn;
    const ToneBrushResult burned = applyToneBrush(request);
    QVERIFY2(!burned.image.isNull(), qPrintable(burned.error));
    const int darkBurn = -changeAt(burned.image, 15);
    const int middleBurn = -changeAt(burned.image, 45);
    const int brightBurn = -changeAt(burned.image, 75);
    QVERIFY(middleBurn > darkBurn + 10);
    QVERIFY(middleBurn > brightBurn + 10);

    for (int y = 0; y < size.height(); ++y) {
        const uchar *before = image.constScanLine(y);
        const uchar *afterDodge = dodged.image.constScanLine(y);
        const uchar *afterBurn = burned.image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(afterDodge[x * 4 + 3], before[x * 4 + 3]);
            QCOMPARE(afterBurn[x * 4 + 3], before[x * 4 + 3]);
        }
    }
}

void CoreTests::dodgeBurnTargetComponentChannelsAndMasks()
{
    QImage channelImage(QSize(24, 24), QImage::Format_RGBA8888);
    for (int y = 0; y < channelImage.height(); ++y) {
        uchar *row = channelImage.scanLine(y);
        for (int x = 0; x < channelImage.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = 31;
            pixel[1] = 77;
            pixel[2] = 143;
            pixel[3] = 220;
        }
    }
    const QImage beforeChannel = channelImage;

    ToneBrushRequest channelRequest;
    channelRequest.destination = channelImage;
    channelRequest.targetSegments = {
        QLineF(QPointF(12.5, 12.5), QPointF(12.5, 12.5))
    };
    channelRequest.diameterPixels = 18.0;
    channelRequest.strength = 0.55;
    channelRequest.hardness = 1.0;
    channelRequest.operation = ToneBrushOperation::Burn;
    channelRequest.range = ToneBrushRange::Highlights;
    channelRequest.target = ToneBrushTarget::ComponentChannel;
    channelRequest.componentIndex = 3;
    const ToneBrushResult channelResult = applyToneBrush(channelRequest);
    QVERIFY2(!channelResult.image.isNull(), qPrintable(channelResult.error));
    const uchar *beforePixel = beforeChannel.constScanLine(12) + 12 * 4;
    const uchar *afterPixel = channelResult.image.constScanLine(12) + 12 * 4;
    QCOMPARE(afterPixel[0], beforePixel[0]);
    QCOMPARE(afterPixel[1], beforePixel[1]);
    QCOMPARE(afterPixel[2], beforePixel[2]);
    QVERIFY(afterPixel[3] < beforePixel[3]);

    QImage mask(QSize(24, 24), QImage::Format_Grayscale8);
    mask.fill(220);
    ToneBrushRequest maskRequest;
    maskRequest.destination = mask;
    maskRequest.targetSegments = channelRequest.targetSegments;
    maskRequest.diameterPixels = 18.0;
    maskRequest.strength = 0.55;
    maskRequest.hardness = 1.0;
    maskRequest.operation = ToneBrushOperation::Burn;
    maskRequest.range = ToneBrushRange::Highlights;
    maskRequest.target = ToneBrushTarget::Mask;
    const ToneBrushResult maskResult = applyToneBrush(maskRequest);
    QVERIFY2(!maskResult.image.isNull(), qPrintable(maskResult.error));
    QVERIFY(maskResult.image.constScanLine(12)[12] < 220);
}

void CoreTests::spongeChangesChromaWhilePreservingLuminanceAndAlpha()
{
    const QSize size(32, 24);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = 165;
            pixel[1] = 112;
            pixel[2] = 96;
            pixel[3] = static_cast<uchar>((x * 37 + y * 19) & 0xff);
        }
    }

    ToneBrushRequest request;
    request.destination = image;
    request.targetSegments = {
        QLineF(QPointF(16.5, 12.5), QPointF(16.5, 12.5))
    };
    request.diameterPixels = 22.0;
    request.strength = 0.5;
    request.hardness = 1.0;
    request.operation = ToneBrushOperation::SpongeSaturate;
    request.target = ToneBrushTarget::RasterRgb;
    request.vibranceProtection = true;
    const ToneBrushResult saturated = applyToneBrush(request);
    QVERIFY2(!saturated.image.isNull(), qPrintable(saturated.error));

    request.operation = ToneBrushOperation::SpongeDesaturate;
    const ToneBrushResult desaturated = applyToneBrush(request);
    QVERIFY2(!desaturated.image.isNull(), qPrintable(desaturated.error));

    const auto sample = [](const QImage &source) {
        const uchar *pixel = source.constScanLine(12) + 16 * 4;
        return std::array<int, 4> {pixel[0], pixel[1], pixel[2], pixel[3]};
    };
    const auto before = sample(image);
    const auto afterSaturate = sample(saturated.image);
    const auto afterDesaturate = sample(desaturated.image);
    const auto chroma = [](const std::array<int, 4> &pixel) {
        return *std::max_element(pixel.begin(), pixel.begin() + 3)
            - *std::min_element(pixel.begin(), pixel.begin() + 3);
    };
    const auto linearValue = [](const double encoded) {
        const double value = encoded / 255.0;
        return value <= 0.04045 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const auto luminanceOf = [&](const std::array<int, 4> &pixel) {
        return 0.2126 * linearValue(pixel[0])
            + 0.7152 * linearValue(pixel[1])
            + 0.0722 * linearValue(pixel[2]);
    };

    QVERIFY(chroma(afterSaturate) > chroma(before));
    QVERIFY(chroma(afterDesaturate) < chroma(before));
    QVERIFY(std::abs(luminanceOf(afterSaturate) - luminanceOf(before)) < 0.015);
    QVERIFY(std::abs(luminanceOf(afterDesaturate) - luminanceOf(before)) < 0.015);
    QCOMPARE(afterSaturate[3], before[3]);
    QCOMPARE(afterDesaturate[3], before[3]);
}

void CoreTests::toneBrushPreservesSixteenBitAlpha()
{
    const QSize size(24, 18);
    QImage image(size, QImage::Format_RGBA64);
    for (int y = 0; y < size.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            row[x] = QRgba64::fromRgba64(
                static_cast<quint16>(15000 + x * 900),
                static_cast<quint16>(21000 + y * 700),
                static_cast<quint16>(18000 + ((x + y) & 3) * 2100),
                static_cast<quint16>((x * 3191 + y * 1877) & 0xffff));
        }
    }
    const QImage before = image;

    ToneBrushRequest request;
    request.destination = image;
    request.targetSegments = {
        QLineF(QPointF(12.5, 9.5), QPointF(12.5, 9.5))
    };
    request.diameterPixels = 16.0;
    request.strength = 0.6;
    request.hardness = 0.8;
    request.operation = ToneBrushOperation::Dodge;
    request.range = ToneBrushRange::Midtones;
    request.target = ToneBrushTarget::RasterRgb;
    const ToneBrushResult result = applyToneBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));

    bool rgbChanged = false;
    for (int y = 0; y < size.height(); ++y) {
        const auto *beforeRow = reinterpret_cast<const QRgba64 *>(before.constScanLine(y));
        const auto *afterRow = reinterpret_cast<const QRgba64 *>(result.image.constScanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(afterRow[x].alpha(), beforeRow[x].alpha());
            if (afterRow[x].red() != beforeRow[x].red()
                || afterRow[x].green() != beforeRow[x].green()
                || afterRow[x].blue() != beforeRow[x].blue()) {
                rgbChanged = true;
            }
        }
    }
    QVERIFY(rgbChanged);
}


void CoreTests::toneBrushHonoursLinearSrgbEncoding()
{
    QImage image(QSize(20, 20), QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    for (int y = 0; y < image.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = 154;
            pixel[1] = 77;
            pixel[2] = 51;
            pixel[3] = 91;
        }
    }

    ToneBrushRequest request;
    request.destination = image;
    request.targetSegments = {
        QLineF(QPointF(10.5, 10.5), QPointF(10.5, 10.5))
    };
    request.diameterPixels = 16.0;
    request.strength = 0.55;
    request.hardness = 1.0;
    request.operation = ToneBrushOperation::SpongeSaturate;
    request.target = ToneBrushTarget::RasterRgb;
    request.vibranceProtection = false;
    const ToneBrushResult result = applyToneBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    QCOMPARE(result.image.colorSpace(), image.colorSpace());

    const uchar *before = image.constScanLine(10) + 10 * 4;
    const uchar *after = result.image.constScanLine(10) + 10 * 4;
    QVERIFY(after[0] != before[0] || after[1] != before[1] || after[2] != before[2]);
    QCOMPARE(after[3], before[3]);
    const double beforeY = 0.2126 * before[0] + 0.7152 * before[1] + 0.0722 * before[2];
    const double afterY = 0.2126 * after[0] + 0.7152 * after[1] + 0.0722 * after[2];
    QVERIFY(std::abs(afterY - beforeY) < 1.0);
}

void CoreTests::toneBrushIncrementalPreviewMatchesWholeGesture()
{
    const QSize size(640, 160);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(40 + (x * 173 / size.width()));
            pixel[1] = static_cast<uchar>(65 + (y * 121 / size.height()));
            pixel[2] = static_cast<uchar>(90 + ((x + y) * 137
                                                / (size.width() + size.height())));
            pixel[3] = static_cast<uchar>((x * 29 + y * 17) & 0xff);
        }
    }

    QVector<QLineF> segments;
    QPointF previous(20.5, 80.5);
    for (int index = 1; index <= 180; ++index) {
        const QPointF next(20.5 + index * 3.0,
                           80.5 + std::sin(index * 0.16) * 28.0);
        segments.push_back(QLineF(previous, next));
        previous = next;
    }

    ToneBrushRequest wholeRequest;
    wholeRequest.destination = image;
    wholeRequest.targetSegments = segments;
    wholeRequest.diameterPixels = 34.0;
    wholeRequest.strength = 0.27;
    wholeRequest.hardness = 0.35;
    wholeRequest.operation = ToneBrushOperation::Dodge;
    wholeRequest.range = ToneBrushRange::Midtones;
    wholeRequest.target = ToneBrushTarget::RasterRgb;
    wholeRequest.protectTones = true;
    const ToneBrushResult whole = applyToneBrush(wholeRequest);
    QVERIFY2(!whole.image.isNull(), qPrintable(whole.error));

    ToneBrushStrokeAccumulator accumulator;
    QImage incremental = image;
    for (const QLineF &segment : segments) {
        ToneBrushRequest append = wholeRequest;
        append.targetSegments = {segment};
        const ToneBrushResult update = applyToneBrushIncremental(
            append, &accumulator, &incremental);
        QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
        // Each update remains local to the newly appended segment instead of
        // expanding with the total length of the active gesture.
        QVERIFY(update.affectedRect.width() <= 48);
        QVERIFY(update.affectedRect.height() <= 82);
    }

    QVERIFY(exactImagesEqual(incremental, whole.image));
    QVERIFY(!accumulator.coverageTiles.isEmpty());
    accumulator.reset();
    QVERIFY(accumulator.coverageTiles.isEmpty());
    QVERIFY(accumulator.imageSize.isEmpty());
}


void CoreTests::blurSharpenPreserveAlphaAndHiddenRgbEdges()
{
    const QSize size(80, 48);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            if (x < 40) {
                pixel[0] = 230;
                pixel[1] = 24;
                pixel[2] = 18;
                pixel[3] = static_cast<uchar>(80 + ((x * 11 + y * 7) % 176));
            } else {
                // Strong hidden green beneath zero Alpha must not contaminate
                // the visible red side of the neighbourhood blur.
                pixel[0] = 8;
                pixel[1] = 245;
                pixel[2] = 21;
                pixel[3] = 0;
            }
        }
    }
    const QImage before = image;

    ToneBrushRequest blur;
    blur.destination = image;
    blur.targetSegments = {
        QLineF(QPointF(39.5, 24.5), QPointF(40.5, 24.5))
    };
    blur.diameterPixels = 34.0;
    blur.strength = 1.0;
    blur.hardness = 1.0;
    blur.operation = ToneBrushOperation::Blur;
    blur.target = ToneBrushTarget::RasterRgb;
    blur.radiusPixels = 6.0;
    const ToneBrushResult blurred = applyToneBrush(blur);
    QVERIFY2(!blurred.image.isNull(), qPrintable(blurred.error));

    for (int y = 0; y < size.height(); ++y) {
        const uchar *beforeRow = before.constScanLine(y);
        const uchar *afterRow = blurred.image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(afterRow[x * 4 + 3], beforeRow[x * 4 + 3]);
        }
    }
    const uchar *visibleEdge = blurred.image.constScanLine(24) + 39 * 4;
    QVERIFY(visibleEdge[0] > 180);
    QVERIFY(visibleEdge[1] < 70);

    // Fully transparent neighbourhoods still retain and smooth hidden RGB.
    const uchar *hiddenBefore = before.constScanLine(24) + 43 * 4;
    const uchar *hiddenAfter = blurred.image.constScanLine(24) + 43 * 4;
    QCOMPARE(hiddenAfter[3], static_cast<uchar>(0));
    QVERIFY(hiddenAfter[0] != hiddenBefore[0]
            || hiddenAfter[1] != hiddenBefore[1]
            || hiddenAfter[2] != hiddenBefore[2]);

    ToneBrushRequest sharpen = blur;
    sharpen.destination = blurred.image;
    sharpen.operation = ToneBrushOperation::Sharpen;
    sharpen.strength = 0.75;
    sharpen.radiusPixels = 3.0;
    sharpen.protectHighlights = true;
    const ToneBrushResult sharpened = applyToneBrush(sharpen);
    QVERIFY2(!sharpened.image.isNull(), qPrintable(sharpened.error));
    for (int y = 0; y < size.height(); ++y) {
        const uchar *beforeRow = blurred.image.constScanLine(y);
        const uchar *afterRow = sharpened.image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            QCOMPARE(afterRow[x * 4 + 3], beforeRow[x * 4 + 3]);
        }
    }
}

void CoreTests::blurSharpenTargetChannelsMasksAndMatchIncrementalPreview()
{
    const QSize size(320, 96);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(30 + (x * 191 / size.width()));
            pixel[1] = static_cast<uchar>(50 + ((x + y * 3) % 170));
            pixel[2] = static_cast<uchar>(210 - (x * 131 / size.width()));
            pixel[3] = static_cast<uchar>((x * 17 + y * 29) & 0xff);
        }
    }

    QVector<QLineF> segments;
    QPointF previous(24.5, 48.5);
    for (int index = 1; index <= 72; ++index) {
        const QPointF next(24.5 + index * 3.5,
                           48.5 + std::sin(index * 0.21) * 22.0);
        segments.push_back(QLineF(previous, next));
        previous = next;
    }

    for (const ToneBrushOperation operation : {ToneBrushOperation::Blur,
                                                ToneBrushOperation::Sharpen}) {
        ToneBrushRequest wholeRequest;
        wholeRequest.destination = image;
        wholeRequest.targetSegments = segments;
        wholeRequest.diameterPixels = 28.0;
        wholeRequest.strength = 0.62;
        wholeRequest.hardness = 0.4;
        wholeRequest.operation = operation;
        wholeRequest.target = ToneBrushTarget::RasterRgb;
        wholeRequest.radiusPixels = operation == ToneBrushOperation::Blur ? 7.0 : 3.0;
        wholeRequest.protectHighlights = true;
        const ToneBrushResult whole = applyToneBrush(wholeRequest);
        QVERIFY2(!whole.image.isNull(), qPrintable(whole.error));

        ToneBrushStrokeAccumulator accumulator;
        QImage incremental = image;
        for (const QLineF &segment : segments) {
            ToneBrushRequest append = wholeRequest;
            append.targetSegments = {segment};
            const ToneBrushResult update = applyToneBrushIncremental(
                append, &accumulator, &incremental);
            QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
            QVERIFY(update.affectedRect.width() <= 40);
            QVERIFY(update.affectedRect.height() <= 72);
        }
        QVERIFY(exactImagesEqual(incremental, whole.image));
    }

    QImage channel = image;
    ToneBrushRequest channelRequest;
    channelRequest.destination = channel;
    channelRequest.targetSegments = {
        QLineF(QPointF(160.5, 48.5), QPointF(160.5, 48.5))
    };
    channelRequest.diameterPixels = 30.0;
    channelRequest.strength = 1.0;
    channelRequest.hardness = 1.0;
    channelRequest.operation = ToneBrushOperation::Blur;
    channelRequest.target = ToneBrushTarget::ComponentChannel;
    channelRequest.componentIndex = 1;
    channelRequest.radiusPixels = 5.0;
    const ToneBrushResult channelResult = applyToneBrush(channelRequest);
    QVERIFY2(!channelResult.image.isNull(), qPrintable(channelResult.error));
    const uchar *beforeChannel = channel.constScanLine(48) + 160 * 4;
    const uchar *afterChannel = channelResult.image.constScanLine(48) + 160 * 4;
    QCOMPARE(afterChannel[0], beforeChannel[0]);
    QCOMPARE(afterChannel[2], beforeChannel[2]);
    QCOMPARE(afterChannel[3], beforeChannel[3]);

    QImage mask(size, QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x] = x < 160 ? 20 : 235;
        }
    }
    ToneBrushRequest maskRequest = channelRequest;
    maskRequest.destination = mask;
    maskRequest.target = ToneBrushTarget::Mask;
    maskRequest.componentIndex = -1;
    const ToneBrushResult maskResult = applyToneBrush(maskRequest);
    QVERIFY2(!maskResult.image.isNull(), qPrintable(maskResult.error));
    QVERIFY(maskResult.image.constScanLine(48)[159] > 20);
    QVERIFY(maskResult.image.constScanLine(48)[160] < 235);
}


void CoreTests::smudgeBrushTransportsStraightRgbaAndFingerColour()
{
    QImage image(128, 64, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    for (int y = 0; y < image.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            uchar *pixel = row + x * 4;
            if (x < 52) {
                pixel[0] = 240;
                pixel[1] = 35;
                pixel[2] = 20;
                pixel[3] = 255;
            } else {
                // Meaningful hidden colour under zero Alpha must travel with
                // transparency rather than being destroyed by premultiplication.
                pixel[0] = 15;
                pixel[1] = 210;
                pixel[2] = 80;
                pixel[3] = 0;
            }
        }
    }

    SmudgeBrushRequest request;
    request.destination = image;
    request.targetSegments = {
        QLineF(QPointF(42.5, 32.5), QPointF(84.5, 32.5))
    };
    request.diameterPixels = 28.0;
    request.strength = 0.85;
    request.hardness = 0.65;
    request.target = SmudgeBrushTarget::RasterRgba;
    const SmudgeBrushResult result = applySmudgeBrush(request);
    QVERIFY2(!result.image.isNull(), qPrintable(result.error));
    QVERIFY(!result.affectedRect.isEmpty());

    const uchar *before = image.constScanLine(32) + 72 * 4;
    const uchar *after = result.image.constScanLine(32) + 72 * 4;
    QCOMPARE(before[3], 0);
    QVERIFY(after[3] > 0);
    QVERIFY(after[0] > after[1]);

    QImage fingerBase(48, 48, QImage::Format_RGBA8888);
    fingerBase.fill(QColor(20, 30, 40, 255));
    SmudgeBrushRequest finger;
    finger.destination = fingerBase;
    finger.targetSegments = {
        QLineF(QPointF(24.5, 24.5), QPointF(24.5, 24.5))
    };
    finger.diameterPixels = 18.0;
    finger.strength = 1.0;
    finger.hardness = 1.0;
    finger.fingerPainting = true;
    finger.fingerColour = QColor(230, 40, 25, 255);
    const SmudgeBrushResult fingerResult = applySmudgeBrush(finger);
    QVERIFY2(!fingerResult.image.isNull(), qPrintable(fingerResult.error));
    const uchar *fingerPixel = fingerResult.image.constScanLine(24) + 24 * 4;
    QVERIFY(fingerPixel[0] > 200);
    QVERIFY(fingerPixel[1] < 80);
    QCOMPARE(fingerPixel[3], 255);

    QImage image64(96, 48, QImage::Format_RGBA64);
    for (int y = 0; y < image64.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(image64.scanLine(y));
        for (int x = 0; x < image64.width(); ++x) {
            row[x] = x < 40
                ? QRgba64::fromRgba64(61000, 5000, 3000, 65535)
                : QRgba64::fromRgba64(4000, 52000, 9000, 0);
        }
    }
    SmudgeBrushRequest request64;
    request64.destination = image64;
    request64.targetSegments = {
        QLineF(QPointF(31.5, 24.5), QPointF(69.5, 24.5))
    };
    request64.diameterPixels = 22.0;
    request64.strength = 0.9;
    request64.hardness = 0.55;
    const SmudgeBrushResult result64 = applySmudgeBrush(request64);
    QVERIFY2(!result64.image.isNull(), qPrintable(result64.error));
    const auto *before64 = reinterpret_cast<const QRgba64 *>(
        image64.constScanLine(24));
    const auto *after64 = reinterpret_cast<const QRgba64 *>(
        result64.image.constScanLine(24));
    QCOMPARE(before64[62].alpha(), quint16(0));
    QVERIFY(after64[62].alpha() > 0);
    QVERIFY(after64[62].red() > after64[62].green());
}

void CoreTests::smudgeBrushTargetsSelectionsChannelsMasksAndMatchesIncremental()
{
    const QSize size(160, 80);
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4 + 0] = static_cast<uchar>(std::clamp(x * 255 / size.width(), 0, 255));
            row[x * 4 + 1] = static_cast<uchar>(x < 70 ? 20 : 220);
            row[x * 4 + 2] = static_cast<uchar>(220 - x * 160 / size.width());
            row[x * 4 + 3] = static_cast<uchar>(80 + x * 175 / size.width());
        }
    }
    const QVector<QLineF> segments {
        QLineF(QPointF(45.5, 38.5), QPointF(78.5, 38.5)),
        QLineF(QPointF(78.5, 38.5), QPointF(112.5, 50.5))
    };

    QImage selection(size, QImage::Format_Grayscale8);
    selection.fill(0);
    for (int y = 10; y < 70; ++y) {
        uchar *row = selection.scanLine(y);
        for (int x = 55; x < 125; ++x) {
            row[x] = 255;
        }
    }

    SmudgeBrushRequest wholeRequest;
    wholeRequest.destination = image;
    wholeRequest.targetSegments = segments;
    wholeRequest.diameterPixels = 24.0;
    wholeRequest.strength = 0.7;
    wholeRequest.hardness = 0.45;
    wholeRequest.target = SmudgeBrushTarget::RasterRgba;
    wholeRequest.selectionCoverage = selection;
    const SmudgeBrushResult whole = applySmudgeBrush(wholeRequest);
    QVERIFY2(!whole.image.isNull(), qPrintable(whole.error));

    SmudgeBrushStrokeState state;
    QImage incremental = image;
    for (const QLineF &segment : segments) {
        SmudgeBrushRequest append = wholeRequest;
        append.targetSegments = {segment};
        const SmudgeBrushResult update = applySmudgeBrushIncremental(
            append, &state, &incremental);
        QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
        QVERIFY(update.image.isNull());
    }
    SmudgeBrushRequest finish = wholeRequest;
    finish.targetSegments.clear();
    finish.finishStroke = true;
    const SmudgeBrushResult finished = applySmudgeBrushIncremental(
        finish, &state, &incremental);
    QVERIFY2(finished.error.isEmpty(), qPrintable(finished.error));
    QVERIFY(finished.image.isNull());
    QVERIFY(exactImagesEqual(incremental, whole.image));
    QCOMPARE(std::memcmp(image.constScanLine(38) + 40 * 4,
                         whole.image.constScanLine(38) + 40 * 4, 4), 0);

    SmudgeBrushRequest channel = wholeRequest;
    channel.destination = image;
    channel.selectionCoverage = {};
    channel.target = SmudgeBrushTarget::ComponentChannel;
    channel.componentIndex = 1;
    const SmudgeBrushResult channelResult = applySmudgeBrush(channel);
    QVERIFY2(!channelResult.image.isNull(), qPrintable(channelResult.error));
    const uchar *beforeChannel = image.constScanLine(40) + 90 * 4;
    const uchar *afterChannel = channelResult.image.constScanLine(40) + 90 * 4;
    QCOMPARE(afterChannel[0], beforeChannel[0]);
    QCOMPARE(afterChannel[2], beforeChannel[2]);
    QCOMPARE(afterChannel[3], beforeChannel[3]);
    QVERIFY(afterChannel[1] != beforeChannel[1]);

    QImage mask(size, QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = mask.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x] = x < 70 ? 20 : 235;
        }
    }
    SmudgeBrushRequest maskRequest = wholeRequest;
    maskRequest.destination = mask;
    maskRequest.selectionCoverage = {};
    maskRequest.target = SmudgeBrushTarget::Mask;
    maskRequest.componentIndex = -1;
    const SmudgeBrushResult maskResult = applySmudgeBrush(maskRequest);
    QVERIFY2(!maskResult.image.isNull(), qPrintable(maskResult.error));
    QVERIFY(maskResult.image.constScanLine(40)[92] < mask.constScanLine(40)[92]);
}

void CoreTests::smudgeBrushLongIncrementalStrokeReusesBoundedScratch()
{
    const QSize size(320, 96);
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = qPremultiply(qRgba((x * 11 + y * 3) & 255,
                                        (x * 5 + y * 17) & 255,
                                        (x * 19 + y * 7) & 255,
                                        80 + ((x * 13 + y * 9) % 176)));
        }
    }

    QVector<QLineF> segments;
    QPointF previous(40.5, 48.5);
    for (int index = 0; index < 180; ++index) {
        const QPointF next(previous.x() + 1.25,
                           48.5 + std::sin(index * 0.13) * 8.0);
        segments.push_back(QLineF(previous, next));
        previous = next;
    }

    SmudgeBrushRequest wholeRequest;
    wholeRequest.destination = source;
    wholeRequest.targetSegments = segments;
    wholeRequest.diameterPixels = 20.0;
    wholeRequest.strength = 0.72;
    wholeRequest.hardness = 0.5;
    const SmudgeBrushResult whole = applySmudgeBrush(wholeRequest);
    QVERIFY2(!whole.image.isNull(), qPrintable(whole.error));
    QCOMPARE(whole.image.format(), QImage::Format_RGBA8888);

    SmudgeBrushStrokeState state;
    QImage incremental = source;
    for (qsizetype index = 0; index < segments.size(); ++index) {
        SmudgeBrushRequest append = wholeRequest;
        append.targetSegments = {segments.at(index)};
        append.finishStroke = index + 1 == segments.size();
        const SmudgeBrushResult update = applySmudgeBrushIncremental(
            append, &state, &incremental);
        QVERIFY2(update.error.isEmpty(), qPrintable(update.error));
        QVERIFY(update.image.isNull());
        QVERIFY(state.scratchImage.width() <= 32);
        QVERIFY(state.scratchImage.height() <= 32);
    }
    // The stateful spacing accumulator must not stamp once per raw event.
    // This 180-event path is roughly 280 px long with 1.6 px spacing.
    QVERIFY(state.appliedDabCount < static_cast<quint64>(segments.size()));
    QCOMPARE(incremental.format(), QImage::Format_RGBA8888);
    QVERIFY(!state.scratchImage.isNull());
    QVERIFY(exactImagesEqual(incremental, whole.image));
}

void CoreTests::layerPlacementSupportsCreatedRasterRedo()
{
    PhotoDocument document;
    QImage source(64, 64, QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(12, 24, 36, 255));
    document.setSourceImage(source);

    const QUuid groupId = document.addGroup();
    QVERIFY(!groupId.isNull());

    LayerNode created;
    created.type = LayerType::Raster;
    created.name = QStringLiteral("Paint Layer");
    const QUuid id = created.id;
    QVERIFY(document.insertLayerAt(created, groupId, 0));

    QUuid parentId;
    int index = -1;
    QVERIFY(document.layerPlacement(id, &parentId, &index));
    QCOMPARE(parentId, groupId);
    QCOMPARE(index, 0);

    QVERIFY(document.removeLayer(id));
    QVERIFY(!document.containsLayer(id));
    QVERIFY(document.insertLayerAt(created, parentId, index));
    QVERIFY(document.containsLayer(id));
    QCOMPARE(document.layerById(id).name, QStringLiteral("Paint Layer"));
    QUuid restoredParent;
    int restoredIndex = -1;
    QVERIFY(document.layerPlacement(id, &restoredParent, &restoredIndex));
    QCOMPARE(restoredParent, groupId);
    QCOMPARE(restoredIndex, 0);
}


void CoreTests::orthogonalTransformPreservesStraightHiddenRgb()
{
    QImage source(2, 3, QImage::Format_RGBA64);
    for (int y = 0; y < source.height(); ++y) {
        auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const quint16 value = static_cast<quint16>(1000 + y * 200 + x * 50);
            row[x] = QRgba64::fromRgba64(value, value + 1, value + 2,
                                         x == 0 && y == 0 ? 0 : 65535);
        }
    }
    const QRgba64 hidden = reinterpret_cast<const QRgba64 *>(
        source.constScanLine(0))[0];
    const QImage rotated = transformImageOrthogonally(
        source, OrthogonalDocumentTransform::Rotate90Clockwise);
    QCOMPARE(rotated.size(), QSize(3, 2));
    const QRgba64 mapped = reinterpret_cast<const QRgba64 *>(
        rotated.constScanLine(0))[2];
    QCOMPARE(mapped.red(), hidden.red());
    QCOMPARE(mapped.green(), hidden.green());
    QCOMPARE(mapped.blue(), hidden.blue());
    QCOMPARE(mapped.alpha(), static_cast<quint16>(0));
}

void CoreTests::orthogonalDocumentTransformUpdatesSelectionGuidesAndResolution()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(4, 3);
    settings.resolutionX = 144.0;
    settings.resolutionY = 96.0;
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    document.setGuides({1.0}, {2.0});
    QImage coverage(4, 3, QImage::Format_Grayscale8);
    coverage.fill(0);
    coverage.scanLine(1)[0] = 255;
    document.selectionMask().setCoverageImage(QRect(0, 0, 4, 3), coverage);

    OrthogonalDocumentTransformResult result;
    QVERIFY2(buildOrthogonalDocumentTransform(
                 document, OrthogonalDocumentTransform::Rotate90Clockwise,
                 &result, &error), qPrintable(error));
    QCOMPARE(result.canvasImage.size(), QSize(3, 4));
    QCOMPARE(result.horizontalGuides, QVector<double>({2.0}));
    QCOMPARE(result.verticalGuides, QVector<double>({2.0}));
    QCOMPARE(result.resolutionX, document.resolutionY());
    QCOMPARE(result.resolutionY, document.resolutionX());
    QCOMPARE(result.layers.size(), document.layers().size());
    QVERIFY(transformsClose(
        result.layers.constFirst().transform,
        document.layers().constFirst().transform
            * documentTransformMatrix(
                OrthogonalDocumentTransform::Rotate90Clockwise,
                settings.pixelSize)));
    QVERIFY(result.layers.constFirst().revision
            > document.layers().constFirst().revision);
    SelectionMask transformed(result.canvasImage.size());
    QVERIFY(transformed.restoreSnapshot(result.selection, false));
    QCOMPARE(transformed.coverageAt(1, 0), static_cast<quint8>(255));
    QCOMPARE(transformed.explicitTileCount(), 1);
}


void CoreTests::orthogonalDocumentTransformRepeatsWithoutAspectScaling()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(5, 3);
    settings.resolutionX = 144.0;
    settings.resolutionY = 96.0;
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    LayerNode layer = document.layers().constFirst();
    layer.rasterImage = QImage(settings.pixelSize, QImage::Format_RGBA8888);
    for (int y = 0; y < layer.rasterImage.height(); ++y) {
        for (int x = 0; x < layer.rasterImage.width(); ++x) {
            layer.rasterImage.setPixelColor(
                x, y, QColor(20 + x * 30, 40 + y * 50, 80 + x + y, 255));
        }
    }
    layer.rasterReferenceOrigin = QPointF();
    layer.rasterReferenceSize = settings.pixelSize;
    layer.transform = QTransform();
    QVector<LayerNode> layers {layer};
    QVERIFY2(document.replaceStructuralState(
                 layer.rasterImage, layers, document.selectionMask().snapshot(),
                 {}, {}, settings.resolutionX, settings.resolutionY, &error),
             qPrintable(error));

    const QImage originalCanvas = document.sourceImage();
    const QImage originalRaster = document.layers().constFirst().rasterImage;
    const QTransform originalTransform = document.layers().constFirst().transform;
    const QSize expectedSizes[] = {
        QSize(3, 5), QSize(5, 3), QSize(3, 5), QSize(5, 3)};

    for (const QSize &expectedSize : expectedSizes) {
        OrthogonalDocumentTransformResult result;
        QVERIFY2(buildOrthogonalDocumentTransform(
                     document, OrthogonalDocumentTransform::Rotate90Clockwise,
                     &result, &error), qPrintable(error));
        QCOMPARE(result.canvasImage.size(), expectedSize);
        QVERIFY2(document.replaceStructuralState(
                     result.canvasImage, result.layers, result.selection,
                     result.horizontalGuides, result.verticalGuides,
                     result.resolutionX, result.resolutionY, &error),
                 qPrintable(error));
    }

    QCOMPARE(document.sourceImage().size(), settings.pixelSize);
    QVERIFY(exactImagesEqual(document.sourceImage(), originalCanvas));
    QVERIFY(exactImagesEqual(document.layers().constFirst().rasterImage,
                             originalRaster));
    QVERIFY(transformsClose(document.layers().constFirst().transform,
                            originalTransform));
    QCOMPARE(document.resolutionX(), settings.resolutionX);
    QCOMPARE(document.resolutionY(), settings.resolutionY);
}

void CoreTests::transformSamplingHonoursNearestAndLanczos()
{
    QImage source(2, 1, QImage::Format_RGBA64);
    auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(0));
    row[0] = QRgba64::fromRgba64(5000, 7000, 9000, 0);
    row[1] = QRgba64::fromRgba64(60000, 50000, 40000, 65535);
    const QRgba64 nearest = sampleTransformRgba64(
        source, source.size(), QPointF(0.25, 0.5),
        TransformInterpolation::NearestNeighbour);
    QCOMPARE(nearest.red(), static_cast<quint16>(5000));
    QCOMPARE(nearest.alpha(), static_cast<quint16>(0));
    const QRgba64 lanczos = sampleTransformRgba64(
        source, source.size(), QPointF(1.0, 0.5),
        TransformInterpolation::Lanczos3);
    QVERIFY(lanczos.red() > nearest.red());
    QVERIFY(lanczos.alpha() > nearest.alpha());
}


void CoreTests::transformSafetyAcceptsBoundedOffCanvasStorage()
{
    const QSize referenceSize(640, 480);
    const QPointF origin(-1200.25, 875.5);
    QTransform transform;
    transform.translate(-250000.0, 125000.0);
    transform.rotate(17.0);

    quint64 preparedBytes = 0;
    TransformStoragePlan plan;
    QString error;
    QVERIFY2(planTransformStorage(referenceSize,
                                  origin,
                                  transform,
                                  referenceSize,
                                  8u,
                                  64u * 1024u * 1024u,
                                  &preparedBytes,
                                  &plan,
                                  &error),
             qPrintable(error));
    QVERIFY(!plan.bounds.isEmpty());
    QVERIFY(plan.bounds.left() < -200000);
    QVERIFY(plan.bounds.top() > 100000);
    QCOMPARE(plan.bytes, preparedBytes);
    QVERIFY(plan.bytes >= static_cast<quint64>(plan.bounds.width())
                            * static_cast<quint64>(plan.bounds.height()) * 8u);
}

void CoreTests::transformSafetyRejectsProjectiveHorizonsAndOversizedPayloads()
{
    QString error;
    // w = 1 - 0.1x crosses zero inside x=[0,20].
    const QTransform horizon(1.0, 0.0, -0.1,
                             0.0, 1.0, 0.0,
                             0.0, 0.0, 1.0);
    QVERIFY(!transformHasSafeDomain(horizon, QRectF(0.0, 0.0, 20.0, 10.0),
                                    1.0e9, &error));
    QVERIFY(error.contains(QStringLiteral("horizon"), Qt::CaseInsensitive));

    QTransform huge;
    huge.scale(400.0, 400.0);
    quint64 preparedBytes = 0;
    TransformStoragePlan plan;
    error.clear();
    QVERIFY(!planTransformStorage(QSize(100, 100), QPointF(), huge,
                                  QSize(100, 100), 4u, 0u,
                                  &preparedBytes, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("32768")));

    QTransform ordinary;
    ordinary.scale(2.0, 2.0);
    preparedBytes = 0;
    error.clear();
    QVERIFY(!planTransformStorage(QSize(2048, 2048), QPointF(), ordinary,
                                  QSize(2048, 2048), 8u,
                                  8u * 1024u * 1024u,
                                  &preparedBytes, &plan, &error));
    QVERIFY(error.contains(QStringLiteral("budget"), Qt::CaseInsensitive));
}

void CoreTests::clipboardPasteDistinguishesSafeEmptyFromFailure()
{
    ClipboardPayload payload;
    payload.imageKind = ClipboardImageKind::Rgba;
    payload.sourceKind = ClipboardSourceKind::RasterPixels;
    payload.image = QImage(2, 2, QImage::Format_RGBA8888);
    payload.image.fill(QColor(180, 40, 90, 255));
    payload.documentBounds = QRect(1, 1, 2, 2);
    payload.sourceDocumentSize = QSize(8, 8);
    payload.hasDocumentPlacement = true;
    QVERIFY(payload.isValid());

    QImage target(8, 8, QImage::Format_RGBA8888);
    target.fill(Qt::transparent);
    const ClipboardPasteResult outside = pasteClipboardIntoRasterTarget(
        payload, target, target.size(), QTransform(), target.size(), false,
        QTransform::fromTranslate(1000.0, 1000.0));
    QVERIFY(outside.succeeded);
    QVERIFY(!outside.changed);
    QVERIFY(!outside.image.isNull());
    QVERIFY(exactImagesEqual(outside.image, target));

    QTransform singular;
    singular.scale(0.0, 1.0);
    const ClipboardPasteResult failed = pasteClipboardIntoRasterTarget(
        payload, target, target.size(), QTransform(), target.size(), false,
        singular);
    QVERIFY(!failed.succeeded);
    QVERIFY(!failed.changed);
    QVERIFY(failed.image.isNull());
}


void CoreTests::clipboardRasterTransformPreservesHiddenRgbAtZeroAlpha()
{
    const QSize extent(6, 4);
    ClipboardPayload payload8;
    payload8.imageKind = ClipboardImageKind::Rgba;
    payload8.sourceKind = ClipboardSourceKind::RasterPixels;
    payload8.image = QImage(1, 1, QImage::Format_RGBA8888);
    uchar *hidden8 = payload8.image.scanLine(0);
    hidden8[0] = 231;
    hidden8[1] = 17;
    hidden8[2] = 99;
    hidden8[3] = 0;
    payload8.documentBounds = QRect(1, 1, 1, 1);
    payload8.sourceDocumentSize = extent;
    payload8.hasDocumentPlacement = true;
    QVERIFY(payload8.isValid());

    QImage target8(extent, QImage::Format_RGBA8888);
    target8.fill(Qt::transparent);
    const ClipboardPasteResult moved8 = pasteClipboardIntoRasterTarget(
        payload8, target8, extent, QTransform(), extent, false,
        QTransform::fromTranslate(2.0, 1.0), TransformInterpolation::NearestNeighbour);
    QVERIFY(moved8.succeeded);
    QVERIFY(moved8.changed);
    const uchar *actual8 = moved8.image.constScanLine(2) + 3 * 4;
    QCOMPARE(actual8[0], static_cast<uchar>(231));
    QCOMPARE(actual8[1], static_cast<uchar>(17));
    QCOMPARE(actual8[2], static_cast<uchar>(99));
    QCOMPARE(actual8[3], static_cast<uchar>(0));

    ClipboardPayload payload16 = payload8;
    payload16.image = QImage(1, 1, QImage::Format_RGBA64);
    auto *hidden16 = reinterpret_cast<QRgba64 *>(payload16.image.scanLine(0));
    hidden16[0] = QRgba64::fromRgba64(61234, 4321, 45678, 0);
    QVERIFY(payload16.isValid());
    QImage target16(extent, QImage::Format_RGBA64);
    target16.fill(Qt::transparent);
    const ClipboardPasteResult moved16 = pasteClipboardIntoRasterTarget(
        payload16, target16, extent, QTransform(), extent, true,
        QTransform::fromTranslate(-1.0, 1.0), TransformInterpolation::NearestNeighbour);
    QVERIFY(moved16.succeeded);
    QVERIFY(moved16.changed);
    const auto *actual16 = reinterpret_cast<const QRgba64 *>(
        moved16.image.constScanLine(2));
    QCOMPARE(actual16[0].red(), static_cast<quint16>(61234));
    QCOMPARE(actual16[0].green(), static_cast<quint16>(4321));
    QCOMPARE(actual16[0].blue(), static_cast<quint16>(45678));
    QCOMPARE(actual16[0].alpha(), static_cast<quint16>(0));
}

void CoreTests::layerJsonRepairsUnsafeTransformMetadata()
{
    LayerNode layer;
    layer.type = LayerType::Group;
    layer.name = QStringLiteral("Unsafe transform");
    QJsonObject object = layer.toJson();
    QJsonObject transform = object.value(QStringLiteral("transform")).toObject();
    transform.insert(QStringLiteral("m11"), 1.0);
    transform.insert(QStringLiteral("m12"), 0.0);
    transform.insert(QStringLiteral("m13"), 0.0);
    transform.insert(QStringLiteral("m21"), 0.0);
    transform.insert(QStringLiteral("m22"), 0.0);
    transform.insert(QStringLiteral("m23"), 0.0);
    transform.insert(QStringLiteral("dx"), 0.0);
    transform.insert(QStringLiteral("dy"), 0.0);
    transform.insert(QStringLiteral("m33"), 1.0);
    object.insert(QStringLiteral("transform"), transform);

    bool ok = false;
    QStringList warnings;
    const LayerNode repaired = LayerNode::fromJson(object, &ok, &warnings);
    QVERIFY(ok);
    QVERIFY(repaired.transform.isIdentity());
    QVERIFY(!warnings.isEmpty());
    QVERIFY(warnings.join(QLatin1Char(' ')).contains(
        QStringLiteral("invalid"), Qt::CaseInsensitive));

    LayerNode unsaveable = layer;
    QTransform enormous;
    enormous.translate(1.0e13, 0.0);
    unsaveable.transform = enormous;
    bool serialised = true;
    QVERIFY(unsaveable.toJson(&serialised).isEmpty());
    QVERIFY(!serialised);
}


void CoreTests::layerJsonRejectsExcessiveHierarchyAndRasterEncoding()
{
    LayerNode leaf;
    leaf.type = LayerType::Raster;
    leaf.name = QStringLiteral("Leaf");
    bool leafOk = false;
    QJsonObject nested = leaf.toJson(&leafOk);
    QVERIFY(leafOk);

    for (int depth = 0; depth < LayerNode::MaximumTreeDepth; ++depth) {
        LayerNode group;
        group.type = LayerType::Group;
        group.name = QStringLiteral("Nested %1").arg(depth);
        bool groupOk = false;
        QJsonObject parent = group.toJson(&groupOk);
        QVERIFY(groupOk);
        parent.insert(QStringLiteral("children"), QJsonArray {nested});
        nested = std::move(parent);
    }

    bool decoded = true;
    const LayerNode rejected = LayerNode::fromJson(nested, &decoded);
    QVERIFY(!decoded);
    Q_UNUSED(rejected);

    LayerNode raster;
    raster.type = LayerType::Raster;
    raster.rasterImage = QImage(2, 2, QImage::Format_RGBA8888);
    raster.rasterImage.fill(QColor(12, 34, 56, 78));
    raster.rasterReferenceSize = raster.rasterImage.size();
    bool serialised = false;
    QJsonObject rasterObject = raster.toJson(&serialised);
    QVERIFY(serialised);
    rasterObject.insert(QStringLiteral("rasterEncoding"), QStringLiteral("raw-dangerous"));
    decoded = true;
    LayerNode::fromJson(rasterObject, &decoded);
    QVERIFY(!decoded);

    rasterObject.insert(QStringLiteral("rasterEncoding"), 42);
    decoded = true;
    LayerNode::fromJson(rasterObject, &decoded);
    QVERIFY(!decoded);

    LayerNode group;
    group.type = LayerType::Group;
    bool groupOk = false;
    QJsonObject invalidChildren = group.toJson(&groupOk);
    QVERIFY(groupOk);
    invalidChildren.insert(QStringLiteral("children"), QStringLiteral("not-an-array"));
    decoded = true;
    LayerNode::fromJson(invalidChildren, &decoded);
    QVERIFY(!decoded);
}

void CoreTests::structuralReplacementRejectsUnsafeTransformMetadata()
{
    NewDocumentSettings settings;
    settings.pixelSize = QSize(12, 9);
    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QImage beforeCanvas = document.sourceImage();
    const QVector<LayerNode> beforeLayers = document.layers();
    const SelectionMask::Snapshot beforeSelection = document.selectionMask().snapshot();

    QVector<LayerNode> unsafeLayers = beforeLayers;
    QVERIFY(!unsafeLayers.isEmpty());
    unsafeLayers[0].transform.scale(0.0, 1.0);
    QVERIFY(!document.replaceLayerTree(unsafeLayers));
    QCOMPARE(document.layers(), beforeLayers);

    error.clear();
    QVERIFY(!document.replaceStructuralState(beforeCanvas,
                                               unsafeLayers,
                                               beforeSelection,
                                               document.horizontalGuides(),
                                               document.verticalGuides(),
                                               document.resolutionX(),
                                               document.resolutionY(),
                                               &error));
    QVERIFY(error.contains(QStringLiteral("unsafe"), Qt::CaseInsensitive));
    QVERIFY(exactImagesEqual(document.sourceImage(), beforeCanvas));
    QCOMPARE(document.layers(), beforeLayers);
    const SelectionMask::Snapshot afterSelection = document.selectionMask().snapshot();
    QCOMPARE(afterSelection.size, beforeSelection.size);
    QCOMPARE(afterSelection.active, beforeSelection.active);
    QCOMPARE(afterSelection.implicitCoverage, beforeSelection.implicitCoverage);
    QCOMPARE(afterSelection.tiles, beforeSelection.tiles);
    QCOMPARE(afterSelection.nonZeroBounds, beforeSelection.nonZeroBounds);
}

void CoreTests::transformWorkflowSurvivesColdResidency()
{
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    DocumentResidencyManager::Limits limits;
    limits.residentDocumentBytes = 64LL * 1024LL * 1024LL;
    limits.warmSessionCount = 0;
    DocumentResidencyManager manager(limits, temporaryDirectory.path());

    DocumentSession first;
    DocumentSession second;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(32, 24);
    QString error;
    QVERIFY2(first.document().createNewDocument(settings, &error), qPrintable(error));
    QVERIFY2(second.document().createNewDocument(settings, &error), qPrintable(error));

    TransformWorkflowState workflow;
    workflow.valid = true;
    workflow.kind = TransformWorkflowKind::LayerTransform;
    workflow.documentTransform = QTransform::fromTranslate(17.0, -9.0);
    workflow.documentTransform.rotate(22.5);
    workflow.sourceBounds = QRectF(2.0, 3.0, 18.0, 12.0);
    workflow.initialPivotDocument = QPointF(11.0, 9.0);
    workflow.finalPivotDocument = QPointF(28.0, 0.0);
    workflow.mode = 2;
    workflow.interpolation = 3;
    workflow.selectedPixels = false;
    first.lastTransform() = workflow;

    manager.registerSession(&first);
    manager.registerSession(&second);
    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY2(manager.activateSession(&second, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Cold);
    QVERIFY(!first.document().hasImage());

    QCOMPARE(first.lastTransform().valid, true);
    QCOMPARE(first.lastTransform().kind, TransformWorkflowKind::LayerTransform);
    QVERIFY(transformsClose(first.lastTransform().documentTransform,
                            workflow.documentTransform));
    QCOMPARE(first.lastTransform().sourceBounds, workflow.sourceBounds);
    QCOMPARE(first.lastTransform().initialPivotDocument,
             workflow.initialPivotDocument);
    QCOMPARE(first.lastTransform().finalPivotDocument,
             workflow.finalPivotDocument);

    QVERIFY2(manager.activateSession(&first, &error), qPrintable(error));
    QVERIFY(first.residency() == SessionResidency::Hot);
    QVERIFY(first.document().hasImage());
    QVERIFY(transformsClose(first.lastTransform().documentTransform,
                            workflow.documentTransform));
    QCOMPARE(first.lastTransform().mode, workflow.mode);
    QCOMPARE(first.lastTransform().interpolation, workflow.interpolation);
}


void CoreTests::typedLevelsRoundTripPreservesVersionSevenAndChannels()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(4, 3, QImage::Format_RGBA64);
    source.fill(QColor(48, 96, 160, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("typed-levels.png"));
    const QUuid levelsId = document.addAdjustment(AdjustmentType::Levels);
    QVERIFY(!levelsId.isNull());

    LevelsParameters expected;
    auto &master = expected.channel(AdjustmentChannel::Rgb);
    master.inputBlack = 0.08;
    master.inputWhite = 0.91;
    master.gamma = 1.35;
    master.outputBlack = 0.04;
    master.outputWhite = 0.96;
    auto &red = expected.channel(AdjustmentChannel::Red);
    red.inputBlack = 0.12;
    red.inputWhite = 0.88;
    red.gamma = 0.82;
    red.outputBlack = 0.03;
    red.outputWhite = 0.79;
    auto &green = expected.channel(AdjustmentChannel::Green);
    green.gamma = 1.18;
    auto &blue = expected.channel(AdjustmentChannel::Blue);
    blue.outputBlack = 0.15;
    blue.outputWhite = 0.92;
    expected.logarithmicHistogram = true;
    expected.autoClipShadows = 0.0025;
    expected.autoClipHighlights = 0.004;
    expected.normalise();
    QVERIFY(document.updateLayer(levelsId, [expected](LayerNode &layer) {
        layer.setLevelsParameters(expected);
    }));

    const QString path = directory.filePath(QStringLiteral("typed-levels.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(json.isObject());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);
    const QJsonArray layers = json.object().value(QStringLiteral("layers")).toArray();
    QVERIFY(!layers.isEmpty());
    const QJsonObject savedLevels = layers.first().toObject();
    QVERIFY(savedLevels.value(QStringLiteral("adjustment")).isObject());
    QCOMPARE(savedLevels.value(QStringLiteral("adjustment")).toObject()
                 .value(QStringLiteral("schema")).toInt(),
             static_cast<int>(AdjustmentData::CurrentSchema));

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(restored.loadWarnings().isEmpty());
    const LevelsParameters actual = restored.layerById(levelsId).effectiveLevelsParameters();
    for (int index = 0; index < 4; ++index) {
        const auto channel = static_cast<AdjustmentChannel>(index);
        const auto &left = actual.channel(channel);
        const auto &right = expected.channel(channel);
        QCOMPARE(left.inputBlack, right.inputBlack);
        QCOMPARE(left.inputWhite, right.inputWhite);
        QCOMPARE(left.gamma, right.gamma);
        QCOMPARE(left.outputBlack, right.outputBlack);
        QCOMPARE(left.outputWhite, right.outputWhite);
    }
    QCOMPARE(actual.logarithmicHistogram, expected.logarithmicHistogram);
    QCOMPARE(actual.autoClipShadows, expected.autoClipShadows);
    QCOMPARE(actual.autoClipHighlights, expected.autoClipHighlights);
}

void CoreTests::legacyLevelsJsonMigratesToTypedPayload()
{
    LayerNode legacy;
    legacy.type = LayerType::Adjustment;
    legacy.adjustmentType = AdjustmentType::Levels;
    legacy.name = QStringLiteral("Legacy Levels");
    legacy.blackPoint = 0.17;
    legacy.whitePoint = 0.83;
    legacy.gamma = 1.45;
    bool encoded = false;
    QJsonObject object = legacy.toJson(&encoded);
    QVERIFY(encoded);
    object.remove(QStringLiteral("adjustment"));

    bool decoded = false;
    const LayerNode restored = LayerNode::fromJson(object, &decoded);
    QVERIFY(decoded);
    const LevelsParameters levels = restored.effectiveLevelsParameters();
    const auto &master = levels.channel(AdjustmentChannel::Rgb);
    QCOMPARE(master.inputBlack, 0.17);
    QCOMPARE(master.inputWhite, 0.83);
    QCOMPARE(master.gamma, 1.45);
    QCOMPARE(master.outputBlack, 0.0);
    QCOMPARE(master.outputWhite, 1.0);
    QVERIFY(levels.channel(AdjustmentChannel::Red) == LevelsChannelParameters {});
    QVERIFY(levels.channel(AdjustmentChannel::Green) == LevelsChannelParameters {});
    QVERIFY(levels.channel(AdjustmentChannel::Blue) == LevelsChannelParameters {});
}

void CoreTests::levelsPerChannelOutputRangesPreserveAlpha()
{
    QImage source(1, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(17, 63, 211, 91));

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();

    LayerNode levels;
    levels.type = LayerType::Adjustment;
    LevelsParameters parameters;
    auto &red = parameters.channel(AdjustmentChannel::Red);
    red.outputBlack = 0.25;
    red.outputWhite = 0.25;
    auto &green = parameters.channel(AdjustmentChannel::Green);
    green.outputBlack = 0.50;
    green.outputWhite = 0.50;
    auto &blue = parameters.channel(AdjustmentChannel::Blue);
    blue.outputBlack = 0.75;
    blue.outputWhite = 0.75;
    levels.setLevelsParameters(parameters);

    const QImage result = ImageProcessor::render(source, {levels, base});
    QVERIFY(!result.isNull());
    const QColor pixel = result.convertToFormat(QImage::Format_RGBA8888).pixelColor(0, 0);
    QCOMPARE(pixel.red(), 64);
    QCOMPARE(pixel.green(), 128);
    QCOMPARE(pixel.blue(), 191);
    QCOMPARE(pixel.alpha(), 91);
}

void CoreTests::typedCurvesRoundTripPreservesVersionSevenAndSchemaThree()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(5, 4, QImage::Format_RGBA64);
    source.fill(QColor(54, 103, 177, 255));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("typed-curves.png"));
    const QUuid curvesId = document.addAdjustment(AdjustmentType::Curves);
    const QUuid exposureId = document.addAdjustment(AdjustmentType::Exposure);
    const QUuid contrastId = document.addAdjustment(AdjustmentType::Contrast);
    const QUuid saturationId = document.addAdjustment(AdjustmentType::Saturation);
    QVERIFY(!curvesId.isNull());
    QVERIFY(!exposureId.isNull());
    QVERIFY(!contrastId.isNull());
    QVERIFY(!saturationId.isNull());

    CurvesParameters expected;
    expected.interpolation = CurveInterpolation::Linear;
    expected.logarithmicHistogram = true;
    expected.channel(AdjustmentChannel::Rgb).points = {
        {0.0, 0.0}, {0.18, 0.08}, {0.63, 0.78}, {1.0, 1.0}
    };
    expected.channel(AdjustmentChannel::Red).points = {
        {0.0, 0.03}, {0.45, 0.58}, {1.0, 0.94}
    };
    expected.channel(AdjustmentChannel::Green).points = {
        {0.0, 0.0}, {0.72, 0.64}, {1.0, 1.0}
    };
    expected.channel(AdjustmentChannel::Blue).points = {
        {0.0, 0.12}, {0.32, 0.24}, {1.0, 0.88}
    };
    expected.normalise();
    QVERIFY(document.updateLayer(curvesId, [expected](LayerNode &layer) {
        layer.setCurvesParameters(expected);
    }));
    const ExposureParameters expectedExposure {1.4, -0.025, 1.35};
    const ContrastParameters expectedContrast {-28.0, 0.37};
    const SaturationParameters expectedSaturation {46.0};
    QVERIFY(document.updateLayer(exposureId, [expectedExposure](LayerNode &layer) {
        layer.setExposureParameters(expectedExposure);
    }));
    QVERIFY(document.updateLayer(contrastId, [expectedContrast](LayerNode &layer) {
        layer.setContrastParameters(expectedContrast);
    }));
    QVERIFY(document.updateLayer(saturationId, [expectedSaturation](LayerNode &layer) {
        layer.setSaturationParameters(expectedSaturation);
    }));

    const QString path = directory.filePath(QStringLiteral("typed-curves.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QVERIFY(json.isObject());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);
    const QJsonArray layers = json.object().value(QStringLiteral("layers")).toArray();
    QJsonObject savedCurves;
    for (const QJsonValue &value : layers) {
        const QJsonObject candidate = value.toObject();
        if (QUuid(candidate.value(QStringLiteral("id")).toString()) == curvesId) {
            savedCurves = candidate;
            break;
        }
    }
    QVERIFY(!savedCurves.isEmpty());
    const QJsonObject typed = savedCurves.value(QStringLiteral("adjustment")).toObject();
    QCOMPARE(typed.value(QStringLiteral("schema")).toInt(),
             static_cast<int>(AdjustmentData::CurrentSchema));
    QCOMPARE(typed.value(QStringLiteral("type")).toString(), QStringLiteral("curves"));

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(restored.loadWarnings().isEmpty());
    const CurvesParameters actual = restored.layerById(curvesId).effectiveCurvesParameters();
    QVERIFY(actual == expected);
    QVERIFY(std::get<ExposureParameters>(
                restored.layerById(exposureId).effectiveAdjustmentData().parameters)
            == expectedExposure);
    QVERIFY(std::get<ContrastParameters>(
                restored.layerById(contrastId).effectiveAdjustmentData().parameters)
            == expectedContrast);
    QVERIFY(std::get<SaturationParameters>(
                restored.layerById(saturationId).effectiveAdjustmentData().parameters)
            == expectedSaturation);
}

void CoreTests::curvesLookupIsExactForEightAndSixteenBit()
{
    CurvesParameters parameters;
    parameters.interpolation = CurveInterpolation::Linear;
    parameters.channel(AdjustmentChannel::Rgb).points = {
        {0.0, 0.0}, {0.5, 0.25}, {1.0, 1.0}
    };
    parameters.channel(AdjustmentChannel::Red).points = {
        {0.0, 0.1}, {1.0, 0.9}
    };
    parameters.channel(AdjustmentChannel::Blue).points = {
        {0.0, 1.0}, {1.0, 0.0}
    };
    parameters.normalise();

    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Curves);
    adjustment.parameters = parameters;
    const TonalLookupTable lookup8 = buildTonalLookup(adjustment, 8);
    const TonalLookupTable lookup16 = buildTonalLookup(adjustment, 16);
    QVERIFY(lookup8.isValid());
    QVERIFY(lookup16.isValid());
    QCOMPARE(lookup8.maximumValue, 255);
    QCOMPARE(lookup16.maximumValue, 65535);

    const int input8 = 128;
    const double normalised8 = input8 / 255.0;
    const double master8 = evaluateCurveChannel(
        parameters.channel(AdjustmentChannel::Rgb), CurveInterpolation::Linear, normalised8);
    const double expectedRed8 = evaluateCurveChannel(
        parameters.channel(AdjustmentChannel::Red), CurveInterpolation::Linear, master8);
    QCOMPARE(lookup8.map(0, input8),
             static_cast<quint16>(std::lround(expectedRed8 * 255.0)));

    const int input16 = 32768;
    const double normalised16 = input16 / 65535.0;
    const double master16 = evaluateCurveChannel(
        parameters.channel(AdjustmentChannel::Rgb), CurveInterpolation::Linear, normalised16);
    const double expectedBlue16 = evaluateCurveChannel(
        parameters.channel(AdjustmentChannel::Blue), CurveInterpolation::Linear, master16);
    QCOMPARE(lookup16.map(2, input16),
             static_cast<quint16>(std::lround(expectedBlue16 * 65535.0)));

    QImage source(1, 1, QImage::Format_RGBA64);
    auto *sourcePixel = reinterpret_cast<QRgba64 *>(source.scanLine(0));
    sourcePixel[0] = QRgba64::fromRgba64(10000, 32768, 50000, 0);
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();
    LayerNode curves;
    curves.type = LayerType::Adjustment;
    curves.setCurvesParameters(parameters);

    const QImage rendered = ImageProcessor::renderPreservingHiddenRgb(source, {curves, base});
    QVERIFY(!rendered.isNull());
    const QImage straight = rendered.convertToFormat(QImage::Format_RGBA64);
    const QRgba64 actual = reinterpret_cast<const QRgba64 *>(straight.constScanLine(0))[0];
    QCOMPARE(actual.red(), lookup16.map(0, 10000));
    QCOMPARE(actual.green(), lookup16.map(1, 32768));
    QCOMPARE(actual.blue(), lookup16.map(2, 50000));
    QCOMPARE(actual.alpha(), quint16(0));

    CurveChannelParameters shaped;
    shaped.points = {{0.0, 0.0}, {0.2, 0.75}, {1.0, 1.0}};
    const double linear = evaluateCurveChannel(shaped, CurveInterpolation::Linear, 0.1);
    const double smooth = evaluateCurveChannel(shaped, CurveInterpolation::Smooth, 0.1);
    QVERIFY(std::abs(linear - smooth) > 1.0e-5);
}

void CoreTests::upgradedAdjustmentsUseTypedParametersAndPreserveIdentity()
{
    QImage source(2, 1, QImage::Format_RGBA64);
    auto *pixels = reinterpret_cast<QRgba64 *>(source.scanLine(0));
    pixels[0] = QRgba64::fromRgba64(8000, 24000, 51000, 0);
    pixels[1] = QRgba64::fromRgba64(12000, 33000, 59000, 42000);
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();

    for (const AdjustmentType type : {AdjustmentType::Exposure,
                                      AdjustmentType::Contrast,
                                      AdjustmentType::Saturation}) {
        LayerNode identity;
        identity.type = LayerType::Adjustment;
        identity.resetAdjustmentParameters(type);
        const QImage rendered = ImageProcessor::renderPreservingHiddenRgb(source, {identity, base});
        QVERIFY(!rendered.isNull());
        const QImage straight = rendered.convertToFormat(QImage::Format_RGBA64);
        for (int x = 0; x < source.width(); ++x) {
            const QRgba64 expected = reinterpret_cast<const QRgba64 *>(source.constScanLine(0))[x];
            const QRgba64 actual = reinterpret_cast<const QRgba64 *>(straight.constScanLine(0))[x];
            QCOMPARE(actual.red(), expected.red());
            QCOMPARE(actual.green(), expected.green());
            QCOMPARE(actual.blue(), expected.blue());
            QCOMPARE(actual.alpha(), expected.alpha());
        }
    }

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    exposure.setExposureParameters({0.0, 0.08, 1.7});
    const ExposureParameters savedExposure = std::get<ExposureParameters>(
        exposure.effectiveAdjustmentData().parameters);
    QCOMPARE(savedExposure.offset, 0.08);
    QCOMPARE(savedExposure.gamma, 1.7);
    const QImage exposed = ImageProcessor::renderPreservingHiddenRgb(source, {exposure, base});
    QVERIFY(!exposed.isNull());
    const QImage exposedStraight = exposed.convertToFormat(QImage::Format_RGBA64);
    QVERIFY(reinterpret_cast<const QRgba64 *>(exposedStraight.constScanLine(0))[1].red()
            != pixels[1].red());

    LayerNode lowPivot;
    lowPivot.type = LayerType::Adjustment;
    lowPivot.setContrastParameters({55.0, 0.20});
    LayerNode highPivot;
    highPivot.type = LayerType::Adjustment;
    highPivot.setContrastParameters({55.0, 0.80});
    const QImage lowResult = ImageProcessor::renderPreservingHiddenRgb(source, {lowPivot, base})
                                 .convertToFormat(QImage::Format_RGBA64);
    const QImage highResult = ImageProcessor::renderPreservingHiddenRgb(source, {highPivot, base})
                                  .convertToFormat(QImage::Format_RGBA64);
    QVERIFY(reinterpret_cast<const QRgba64 *>(lowResult.constScanLine(0))[1].green()
            != reinterpret_cast<const QRgba64 *>(highResult.constScanLine(0))[1].green());

    LayerNode saturation;
    saturation.type = LayerType::Adjustment;
    saturation.setSaturationParameters({-100.0});
    const QImage neutral = ImageProcessor::renderPreservingHiddenRgb(source, {saturation, base})
                               .convertToFormat(QImage::Format_RGBA64);
    const QRgba64 neutralPixel = reinterpret_cast<const QRgba64 *>(neutral.constScanLine(0))[1];
    QVERIFY(std::abs(int(neutralPixel.red()) - int(neutralPixel.green())) <= 2);
    QVERIFY(std::abs(int(neutralPixel.green()) - int(neutralPixel.blue())) <= 2);
    QCOMPARE(neutralPixel.alpha(), quint16(42000));
}

void CoreTests::schemaOneAdjustmentsMigrateWithProfessionalDefaults()
{
    auto decode = [](const QString &type, const QJsonObject &parameters) {
        QJsonObject object;
        object.insert(QStringLiteral("schema"), 1);
        object.insert(QStringLiteral("type"), type);
        object.insert(QStringLiteral("parameters"), parameters);
        bool ok = false;
        AdjustmentData result = AdjustmentData::fromJson(
            object, AdjustmentType::Exposure, &ok);
        return std::pair<AdjustmentData, bool>(std::move(result), ok);
    };

    const auto [exposure, exposureOk] = decode(
        QStringLiteral("exposure"), {{QStringLiteral("exposure"), 1.25}});
    QVERIFY(exposureOk);
    const auto exposureParameters = std::get<ExposureParameters>(exposure.parameters);
    QCOMPARE(exposure.schema, AdjustmentData::CurrentSchema);
    QCOMPARE(exposureParameters.exposure, 1.25);
    QCOMPARE(exposureParameters.offset, 0.0);
    QCOMPARE(exposureParameters.gamma, 1.0);

    const auto [contrast, contrastOk] = decode(
        QStringLiteral("contrast"), {{QStringLiteral("contrast"), -32.0}});
    QVERIFY(contrastOk);
    const auto contrastParameters = std::get<ContrastParameters>(contrast.parameters);
    QCOMPARE(contrastParameters.contrast, -32.0);
    QCOMPARE(contrastParameters.pivot, 0.5);
}

void CoreTests::discreteAdjustmentsAreExactAtEightAndSixteenBit()
{
    QImage source8(2, 1, QImage::Format_RGBA8888);
    source8.setPixelColor(0, 0, QColor(64, 130, 250, 0));
    source8.setPixelColor(1, 0, QColor(192, 10, 128, 173));
    LayerNode base8;
    base8.type = LayerType::BaseImage;
    base8.rasterImage = source8;
    base8.rasterReferenceSize = source8.size();

    LayerNode posterise;
    posterise.type = LayerType::Adjustment;
    PosteriseParameters posteriseParameters;
    posteriseParameters.levels = 4;
    posterise.setPosteriseParameters(posteriseParameters);
    const QImage posterised8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {posterise, base8}).convertToFormat(QImage::Format_RGBA8888);
    const auto expectedPosterise8 = [](const int code) {
        return qRound(std::round((code / 255.0) * 3.0) / 3.0 * 255.0);
    };
    QCOMPARE(posterised8.pixelColor(0, 0).red(), expectedPosterise8(64));
    QCOMPARE(posterised8.pixelColor(0, 0).green(), expectedPosterise8(130));
    QCOMPARE(posterised8.pixelColor(0, 0).blue(), expectedPosterise8(250));
    QCOMPARE(posterised8.pixelColor(0, 0).alpha(), 0);

    LayerNode threshold;
    threshold.type = LayerType::Adjustment;
    ThresholdParameters thresholdParameters;
    thresholdParameters.source = ThresholdSource::Red;
    thresholdParameters.threshold = 128.0 / 255.0;
    threshold.setThresholdParameters(thresholdParameters);
    const QImage thresholded8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {threshold, base8}).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(thresholded8.pixelColor(0, 0), QColor(0, 0, 0, 0));
    QCOMPARE(thresholded8.pixelColor(1, 0), QColor(255, 255, 255, 173));

    QImage source16(2, 1, QImage::Format_RGBA64);
    auto *input16 = reinterpret_cast<QRgba64 *>(source16.bits());
    input16[0] = QRgba64::fromRgba64(10'000, 32'000, 60'000, 0);
    input16[1] = QRgba64::fromRgba64(40'000, 2'000, 30'000, 47'000);
    LayerNode base16;
    base16.type = LayerType::BaseImage;
    base16.rasterImage = source16;
    base16.rasterReferenceSize = source16.size();

    posteriseParameters.levels = 5;
    posterise.setPosteriseParameters(posteriseParameters);
    const QImage posterised16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {posterise, base16}).convertToFormat(QImage::Format_RGBA64);
    const auto *posterisedOutput16 = reinterpret_cast<const QRgba64 *>(posterised16.constBits());
    const auto expectedPosterise16 = [](const quint16 code) {
        return static_cast<quint16>(std::lround(
            std::round((code / 65535.0) * 4.0) / 4.0 * 65535.0));
    };
    QCOMPARE(posterisedOutput16[0],
             QRgba64::fromRgba64(expectedPosterise16(10'000),
                                 expectedPosterise16(32'000),
                                 expectedPosterise16(60'000), 0));

    thresholdParameters.threshold = 0.5;
    threshold.setThresholdParameters(thresholdParameters);
    const QImage thresholded16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {threshold, base16}).convertToFormat(QImage::Format_RGBA64);
    const auto *output16 = reinterpret_cast<const QRgba64 *>(thresholded16.constBits());
    QCOMPARE(output16[0], QRgba64::fromRgba64(0, 0, 0, 0));
    QCOMPARE(output16[1], QRgba64::fromRgba64(65'535, 65'535, 65'535, 47'000));
}

void CoreTests::cubeLutParserSupportsOneThreeAndCombinedTables()
{
    const QByteArray oneD = R"CUBE(
TITLE "Test 1D"
LUT_1D_SIZE 2
0.0 0.2 0.4
1.0 0.8 0.6
)CUBE";
    LutParameters oneDParameters;
    QString error;
    QVERIFY2(CubeLut::parse(oneD, QStringLiteral("one.cube"), &oneDParameters, &error),
             qPrintable(error));
    QVERIFY(oneDParameters.hasShaper());
    QVERIFY(!oneDParameters.hasCube());
    const auto oneDResult = CubeLut::evaluate(oneDParameters, {0.25, 0.5, 0.75});
    QVERIFY(std::abs(oneDResult[0] - 0.25) < 1.0e-6);
    QVERIFY(std::abs(oneDResult[1] - 0.50) < 1.0e-6);
    QVERIFY(std::abs(oneDResult[2] - 0.55) < 1.0e-6);

    const QByteArray identity3D = R"CUBE(
TITLE "Identity 3D"
LUT_3D_SIZE 2
0 0 0
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)CUBE";
    LutParameters cubeParameters;
    QVERIFY2(CubeLut::parse(identity3D, QStringLiteral("identity.cube"),
                            &cubeParameters, &error), qPrintable(error));
    QVERIFY(!cubeParameters.hasShaper());
    QVERIFY(cubeParameters.hasCube());
    const auto cubeResult = CubeLut::evaluate(cubeParameters, {0.2, 0.45, 0.8});
    QVERIFY(std::abs(cubeResult[0] - 0.2) < 1.0e-6);
    QVERIFY(std::abs(cubeResult[1] - 0.45) < 1.0e-6);
    QVERIFY(std::abs(cubeResult[2] - 0.8) < 1.0e-6);

    const QByteArray combined = R"CUBE(
TITLE "Combined"
LUT_1D_SIZE 2
LUT_3D_SIZE 2
0 0 0
0.5 0.5 0.5
0 0 0
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)CUBE";
    LutParameters combinedParameters;
    QVERIFY2(CubeLut::parse(combined, QStringLiteral("combined.cube"),
                            &combinedParameters, &error), qPrintable(error));
    QVERIFY(combinedParameters.hasShaper());
    QVERIFY(combinedParameters.hasCube());
    const auto combinedResult = CubeLut::evaluate(combinedParameters, {0.4, 0.6, 0.8});
    QVERIFY(std::abs(combinedResult[0] - 0.2) < 1.0e-6);
    QVERIFY(std::abs(combinedResult[1] - 0.3) < 1.0e-6);
    QVERIFY(std::abs(combinedResult[2] - 0.4) < 1.0e-6);
    QVERIFY(combinedParameters.tableFingerprint != 0);
}

void CoreTests::malformedCubeLutIsRejectedSafely()
{
    LutParameters parameters;
    QString error;
    QVERIFY(!CubeLut::parse(QByteArray("LUT_3D_SIZE 2\n0 0 0\n"),
                            QStringLiteral("short.cube"), &parameters, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!CubeLut::parse(QByteArray("LUT_1D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 0 0 0\n0 0 0\n1 1 1\n"),
                            QStringLiteral("domain.cube"), &parameters, &error));
    QVERIFY(!error.isEmpty());
}

void CoreTests::lutAdjustmentPreservesStrengthAlphaAndPrecision()
{
    LutParameters parameters;
    parameters.title = QStringLiteral("Invert");
    parameters.shaperSize = 2;
    parameters.shaperData = {1.0f, 1.0f, 1.0f,
                             0.0f, 0.0f, 0.0f};
    parameters.strength = 50.0;
    parameters.normalise();
    QVERIFY(parameters.hasShaper());
    QVERIFY(parameters.gpuDisplayRangeCompatible);

    LayerNode lut;
    lut.type = LayerType::Adjustment;
    lut.setLutParameters(parameters);

    QImage source8(2, 1, QImage::Format_RGBA8888);
    source8.setPixelColor(0, 0, QColor(32, 80, 144, 0));
    source8.setPixelColor(1, 0, QColor(255, 0, 128, 173));
    LayerNode base8;
    base8.type = LayerType::BaseImage;
    base8.rasterImage = source8;
    base8.rasterReferenceSize = source8.size();
    const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {lut, base8}).convertToFormat(QImage::Format_RGBA8888);
    const auto halfInvert8 = [](const int code) {
        return qRound((code / 255.0 + (1.0 - code / 255.0)) * 0.5 * 255.0);
    };
    QCOMPARE(output8.pixelColor(0, 0),
             QColor(halfInvert8(32), halfInvert8(80), halfInvert8(144), 0));
    QCOMPARE(output8.pixelColor(1, 0),
             QColor(halfInvert8(255), halfInvert8(0), halfInvert8(128), 173));

    QImage source16(2, 1, QImage::Format_RGBA64);
    auto *pixels16 = reinterpret_cast<QRgba64 *>(source16.bits());
    pixels16[0] = QRgba64::fromRgba64(7'000, 24'000, 59'000, 0);
    pixels16[1] = QRgba64::fromRgba64(65'535, 0, 32'768, 49'123);
    LayerNode base16;
    base16.type = LayerType::BaseImage;
    base16.rasterImage = source16;
    base16.rasterReferenceSize = source16.size();
    const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {lut, base16}).convertToFormat(QImage::Format_RGBA64);
    const auto *result16 = reinterpret_cast<const QRgba64 *>(output16.constBits());
    const auto halfInvert16 = [](const quint16 code) {
        return static_cast<quint16>(std::lround(
            (code / 65535.0 + (1.0 - code / 65535.0)) * 0.5 * 65535.0));
    };
    QCOMPARE(result16[0],
             QRgba64::fromRgba64(halfInvert16(7'000), halfInvert16(24'000),
                                 halfInvert16(59'000), 0));
    QCOMPARE(result16[1],
             QRgba64::fromRgba64(halfInvert16(65'535), halfInvert16(0),
                                 halfInvert16(32'768), 49'123));
}

void CoreTests::extendedRangeLutUsesFloatingPointGpuPayload()
{
    const QByteArray extended = R"CUBE(
TITLE "Extended"
LUT_1D_SIZE 2
-0.25 0.0 0.0
1.25 1.0 1.0
)CUBE";
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::parse(extended, QStringLiteral("extended.cube"),
                            &parameters, &error), qPrintable(error));
    QVERIFY(parameters.hasShaper());
    QVERIFY(!parameters.gpuDisplayRangeCompatible);
    QVERIFY(parameters.gpuHalfFloatCompatible);
    const LutGpuTextureData texture =
        CubeLut::buildGpuTextureData(parameters, &error);
    QVERIFY2(texture.isValid(), qPrintable(error));
    QCOMPARE(float(texture.rgba16f.at(0)), -0.25f);
    QCOMPARE(float(texture.rgba16f.at(4)), 1.25f);

    parameters.strength = 50.0;
    parameters.normalise();
    const auto mapped = CubeLut::evaluate(parameters, {0.0, 0.5, 1.0});
    // The authoritative evaluator retains extended output after Strength.
    // Integer image destinations clamp only when the pixel is written.
    QVERIFY(std::abs(mapped[0] - (-0.125)) < 1.0e-9);
    QVERIFY(std::abs(mapped[1] - 0.5) < 1.0e-9);
    QVERIFY(std::abs(mapped[2] - 1.0) < 1.0e-9);

    LayerNode lut;
    lut.type = LayerType::Adjustment;
    lut.setLutParameters(parameters);
    QImage source(1, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(0, 128, 255, 91));
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();
    const QImage rendered = ImageProcessor::renderPreservingHiddenRgb(
        source, {lut, base}).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(rendered.pixelColor(0, 0).red(), 0);
    QCOMPARE(rendered.pixelColor(0, 0).alpha(), 91);

    AdjustmentData data;
    data.reset(AdjustmentType::Lut);
    data.parameters = parameters;
    bool encodedOk = false;
    const QJsonObject encoded = data.toJson(&encodedOk);
    QVERIFY(encodedOk);
    bool decodedOk = false;
    const AdjustmentData decoded = AdjustmentData::fromJson(
        encoded, AdjustmentType::Lut, &decodedOk);
    QVERIFY(decodedOk);
    const LutParameters restored = std::get<LutParameters>(decoded.parameters);
    QVERIFY(!restored.gpuDisplayRangeCompatible);
    QCOMPARE(restored.interpolation, LutInterpolation::Tetrahedral);
    QCOMPARE(restored.tableFingerprint, parameters.tableFingerprint);
}

void CoreTests::lutAdjustmentRoundTripsEmbeddedDataInCurrentSchema()
{
    const QByteArray identity3D = R"CUBE(
TITLE "Embedded Identity"
LUT_3D_SIZE 2
0 0 0
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)CUBE";
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::parse(identity3D, QStringLiteral("removed-after-import.cube"),
                            &parameters, &error), qPrintable(error));
    parameters.strength = 63.0;
    parameters.normalise();

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage source(3, 2, QImage::Format_RGBA64);
    source.fill(QColor(45, 93, 177, 121));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("lut-source.png"));
    const QUuid id = document.addAdjustment(AdjustmentType::Lut);
    QVERIFY(!id.isNull());
    QVERIFY(document.updateLayer(id, [parameters](LayerNode &layer) {
        layer.setLutParameters(parameters);
    }));

    const QString path = directory.filePath(QStringLiteral("embedded-lut.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument encoded = QJsonDocument::fromJson(file.readAll());
    QCOMPARE(encoded.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);
    const QJsonArray layers = encoded.object().value(QStringLiteral("layers")).toArray();
    bool foundCurrentSchema = false;
    for (const QJsonValue &layerValue : layers) {
        const QJsonObject adjustment = layerValue.toObject()
            .value(QStringLiteral("adjustment")).toObject();
        if (adjustment.value(QStringLiteral("type")).toString() == QStringLiteral("lut")) {
            foundCurrentSchema = adjustment.value(QStringLiteral("schema")).toInt()
                == static_cast<int>(AdjustmentData::CurrentSchema);
            const QJsonObject payload = adjustment.value(QStringLiteral("parameters")).toObject();
            QVERIFY(!payload.value(QStringLiteral("cubeData")).toString().isEmpty());
            QCOMPARE(payload.value(QStringLiteral("interpolation")).toString(),
                     QStringLiteral("tetrahedral"));
            QCOMPARE(payload.value(QStringLiteral("processingMode")).toString(),
                     QStringLiteral("encoded-document"));
            QCOMPARE(payload.value(QStringLiteral("operatorProfile")).toString(),
                     QStringLiteral("generic"));
        }
    }
    QVERIFY(foundCurrentSchema);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    const LutParameters restoredParameters = std::get<LutParameters>(
        restored.layerById(id).effectiveAdjustmentData().parameters);
    QVERIFY(restoredParameters == parameters);
    QVERIFY(restoredParameters.hasCube());
    QVERIFY(restoredParameters.tableFingerprint != 0);
}

void CoreTests::builtInAdjustmentPresetsMatchTypeAndSchema()
{
    for (int raw = static_cast<int>(AdjustmentType::Exposure);
         raw <= static_cast<int>(AdjustmentType::RadialBlur); ++raw) {
        const auto type = static_cast<AdjustmentType>(raw);
        const QVector<AdjustmentPreset> presets = AdjustmentPresetStore::builtInPresets(type);
        for (const AdjustmentPreset &preset : presets) {
            QVERIFY2(!preset.name.trimmed().isEmpty(), qPrintable(defaultAdjustmentName(type)));
            QVERIFY(preset.builtIn);
            QCOMPARE(preset.adjustment.type, type);
            QCOMPARE(preset.adjustment.schema, AdjustmentData::CurrentSchema);
            bool serialised = false;
            const QJsonObject json = preset.adjustment.toJson(&serialised);
            QVERIFY(serialised);
            bool restored = false;
            const AdjustmentData decoded = AdjustmentData::fromJson(json, type, &restored);
            QVERIFY(restored);
            QVERIFY(decoded == preset.adjustment);
        }
    }
}

void CoreTests::adjustmentPresetStoreRoundTripsUserPreset()
{
    QStandardPaths::setTestModeEnabled(true);
    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Posterise);
    PosteriseParameters parameters;
    parameters.levels = 11;
    adjustment.parameters = parameters;
    adjustment.normalise();
    const QString uniqueName = QStringLiteral("Core Test %1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString error;
    QVERIFY2(AdjustmentPresetStore::saveUserPreset(uniqueName, adjustment, &error),
             qPrintable(error));
    const QVector<AdjustmentPreset> presets = AdjustmentPresetStore::presets(
        AdjustmentType::Posterise);
    auto found = std::find_if(presets.cbegin(), presets.cend(),
                             [&uniqueName](const AdjustmentPreset &preset) {
        return !preset.builtIn && preset.name == uniqueName;
    });
    QVERIFY(found != presets.cend());
    QVERIFY(found->adjustment == adjustment);
    const AdjustmentPreset saved = *found;
    QVERIFY2(AdjustmentPresetStore::removeUserPreset(saved, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(saved.storagePath));
}

void CoreTests::shadowsHighlightsRoundTripPreservesVersionSevenAndSchemaSix()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage source(37, 29, QImage::Format_RGBA64);
    source.fill(QColor(51, 102, 177, 143));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("shadows-highlights-source.png"));
    const QUuid id = document.addAdjustment(AdjustmentType::ShadowsHighlights);
    QVERIFY(!id.isNull());

    ShadowsHighlightsParameters expected;
    expected.shadowAmount = 43.0;
    expected.shadowTonalWidth = 61.0;
    expected.highlightAmount = 38.0;
    expected.highlightTonalWidth = 54.0;
    expected.radius = 137.0;
    expected.midtoneContrast = 17.0;
    expected.colourCorrection = 31.0;
    expected.normalise();
    QVERIFY(document.updateLayer(id, [expected](LayerNode &layer) {
        layer.setShadowsHighlightsParameters(expected);
    }));

    const QString path = directory.filePath(QStringLiteral("shadows-highlights.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument encoded = QJsonDocument::fromJson(file.readAll());
    QCOMPARE(encoded.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);
    const QJsonArray layers = encoded.object().value(QStringLiteral("layers")).toArray();
    QVERIFY(!layers.isEmpty());
    const QJsonObject adjustment = layers.first().toObject()
        .value(QStringLiteral("adjustment")).toObject();
    QCOMPARE(adjustment.value(QStringLiteral("type")).toString(),
             QStringLiteral("shadows-highlights"));
    QCOMPARE(adjustment.value(QStringLiteral("schema")).toInt(),
             static_cast<int>(AdjustmentData::CurrentSchema));

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    const LayerNode restoredLayer = restored.layerById(id);
    QVERIFY(!restoredLayer.id.isNull());
    QCOMPARE(restoredLayer.adjustmentType, AdjustmentType::ShadowsHighlights);
    const auto actual = std::get<ShadowsHighlightsParameters>(
        restoredLayer.effectiveAdjustmentData().parameters);
    QVERIFY(actual == expected);
}

void CoreTests::shadowsHighlightsIdentityAndRecoveryPreservePrecisionAndAlpha()
{
    for (const QImage::Format format : {QImage::Format_RGBA8888,
                                        QImage::Format_RGBA64}) {
        QImage source(64, 24, format);
        if (format == QImage::Format_RGBA64) {
            for (int y = 0; y < source.height(); ++y) {
                auto *row = reinterpret_cast<QRgba64 *>(source.scanLine(y));
                for (int x = 0; x < source.width(); ++x) {
                    const bool dark = x < source.width() / 2;
                    row[x] = QRgba64::fromRgba64(
                        dark ? quint16(5'500 + y * 13) : quint16(57'000 - y * 17),
                        dark ? quint16(8'000 + y * 11) : quint16(54'000 - y * 13),
                        dark ? quint16(12'000 + y * 7) : quint16(49'000 - y * 9),
                        (x == 3 && y == 4) ? quint16(0) : quint16(42'000));
                }
            }
        } else {
            for (int y = 0; y < source.height(); ++y) {
                for (int x = 0; x < source.width(); ++x) {
                    const bool dark = x < source.width() / 2;
                    source.setPixelColor(x, y, QColor(
                        dark ? 24 + y / 3 : 228 - y / 4,
                        dark ? 32 + y / 4 : 215 - y / 5,
                        dark ? 45 + y / 5 : 198 - y / 6,
                        (x == 3 && y == 4) ? 0 : 173));
                }
            }
        }

        LayerNode base;
        base.type = LayerType::BaseImage;
        base.rasterImage = source;
        base.rasterReferenceSize = source.size();
        LayerNode adjustment;
        adjustment.type = LayerType::Adjustment;
        adjustment.resetAdjustmentParameters(AdjustmentType::ShadowsHighlights);

        const QImage identity = ImageProcessor::renderPreservingHiddenRgb(
            source, {adjustment, base}).convertToFormat(format);
        QVERIFY(exactImagesEqual(identity, source));

        ShadowsHighlightsParameters parameters;
        parameters.shadowAmount = 55.0;
        parameters.shadowTonalWidth = 62.0;
        parameters.highlightAmount = 47.0;
        parameters.highlightTonalWidth = 58.0;
        parameters.radius = 11.0;
        parameters.midtoneContrast = 8.0;
        parameters.colourCorrection = 24.0;
        adjustment.setShadowsHighlightsParameters(parameters);
        const QImage recovered = ImageProcessor::renderPreservingHiddenRgb(
            source, {adjustment, base}).convertToFormat(format);
        QVERIFY(!recovered.isNull());

        if (format == QImage::Format_RGBA64) {
            const auto *before = reinterpret_cast<const QRgba64 *>(source.constBits());
            const auto *after = reinterpret_cast<const QRgba64 *>(recovered.constBits());
            const int darkIndex = 12 * source.width() + 10;
            const int brightIndex = 12 * source.width() + 53;
            QVERIFY(after[darkIndex].red() > before[darkIndex].red());
            QVERIFY(after[brightIndex].red() < before[brightIndex].red());
            QCOMPARE(after[4 * source.width() + 3].alpha(), quint16(0));
            QVERIFY(after[4 * source.width() + 3].red() > 0);
            for (int index = 0; index < source.width() * source.height(); ++index) {
                QCOMPARE(after[index].alpha(), before[index].alpha());
            }
        } else {
            QVERIFY(recovered.pixelColor(10, 12).red()
                    > source.pixelColor(10, 12).red());
            QVERIFY(recovered.pixelColor(53, 12).red()
                    < source.pixelColor(53, 12).red());
            QCOMPARE(recovered.pixelColor(3, 4).alpha(), 0);
            QVERIFY(recovered.pixelColor(3, 4).red() > 0);
            for (int y = 0; y < source.height(); ++y) {
                for (int x = 0; x < source.width(); ++x) {
                    QCOMPARE(recovered.pixelColor(x, y).alpha(),
                             source.pixelColor(x, y).alpha());
                }
            }
        }
    }
}

void CoreTests::shadowsHighlightsCancellationPublishesNoPartialResult()
{
    const QSize size(96, 80);
    QImage source(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            source.setPixelColor(x, y, QColor((x * 7 + y * 3) % 256,
                                              (x * 5 + y * 11) % 256,
                                              (x * 13 + y * 2) % 256,
                                              (x + y) % 9 == 0 ? 0 : 211));
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = size;
    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    ShadowsHighlightsParameters parameters;
    parameters.shadowAmount = 63.0;
    parameters.highlightAmount = 52.0;
    parameters.radius = 37.0;
    parameters.midtoneContrast = 17.0;
    adjustment.setShadowsHighlightsParameters(parameters);

    std::atomic_bool cancelled {true};
    const QImage result = ImageProcessor::renderRegion(
        source, {adjustment, base}, source.rect(), size, &cancelled);
    QVERIFY(result.isNull());
}

void CoreTests::shadowsHighlightsTiledRegionsMatchFullRenderAcrossBoundaries()
{
    const QSize size(600, 520);
    QImage source(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const bool left = x < 256;
            const bool upper = y < 256;
            source.setPixelColor(x, y, QColor(
                left ? 22 + (y % 29) : 221 - (y % 31),
                upper ? 35 + (x % 37) : 214 - (x % 41),
                (x + y) % 97 + 71,
                ((x + y) % 19 == 0) ? 0 : 193));
        }
    }
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();
    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    ShadowsHighlightsParameters parameters;
    parameters.shadowAmount = 49.0;
    parameters.shadowTonalWidth = 68.0;
    parameters.highlightAmount = 41.0;
    parameters.highlightTonalWidth = 57.0;
    parameters.radius = 83.0;
    parameters.midtoneContrast = 13.0;
    parameters.colourCorrection = 27.0;
    adjustment.setShadowsHighlightsParameters(parameters);
    LayerNode upperAdjustment;
    upperAdjustment.type = LayerType::Adjustment;
    ShadowsHighlightsParameters upperParameters;
    upperParameters.shadowAmount = 18.0;
    upperParameters.shadowTonalWidth = 52.0;
    upperParameters.highlightAmount = 23.0;
    upperParameters.highlightTonalWidth = 49.0;
    upperParameters.radius = 47.0;
    upperParameters.midtoneContrast = -6.0;
    upperParameters.colourCorrection = 13.0;
    upperAdjustment.setShadowsHighlightsParameters(upperParameters);
    const QVector<LayerNode> layers {upperAdjustment, adjustment, base};
    QCOMPARE(maximumSpatialAdjustmentRadius(layers), 130);

    const QImage full = ImageProcessor::renderRegion(
        source, layers, source.rect(), source.size());
    QVERIFY(!full.isNull());
    QImage assembled(full.size(), full.format());
    assembled.fill(Qt::transparent);
    assembled.setColorSpace(full.colorSpace());
    assembled.setDevicePixelRatio(full.devicePixelRatio());
    assembled.setDotsPerMeterX(full.dotsPerMeterX());
    assembled.setDotsPerMeterY(full.dotsPerMeterY());

    for (int top = 0; top < size.height(); top += 256) {
        for (int left = 0; left < size.width(); left += 256) {
            const QRect region(left, top,
                               std::min(256, size.width() - left),
                               std::min(256, size.height() - top));
            const QImage tile = ImageProcessor::renderRegion(
                source, layers, region, source.size());
            QVERIFY(!tile.isNull());
            QCOMPARE(tile.size(), region.size());
            QCOMPARE(tile.format(), assembled.format());
            const qsizetype activeBytes = static_cast<qsizetype>(tile.width())
                * tile.depth() / 8;
            for (int row = 0; row < tile.height(); ++row) {
                std::memcpy(assembled.scanLine(top + row)
                                + static_cast<qsizetype>(left) * tile.depth() / 8,
                            tile.constScanLine(row),
                            static_cast<std::size_t>(activeBytes));
            }
        }
    }
    QVERIFY(exactImagesEqual(assembled, full));

    const QImage hiddenFull = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, layers, source.rect(), source.size());
    const QImage hiddenCrossing = ImageProcessor::renderRegionPreservingHiddenRgb(
        source, layers, QRect(211, 203, 173, 161), source.size());
    QVERIFY(!hiddenFull.isNull());
    QVERIFY(!hiddenCrossing.isNull());
    const QImage hiddenReference = hiddenFull.copy(QRect(211, 203, 173, 161));
    QVERIFY(exactImagesEqual(hiddenCrossing, hiddenReference));

    // Exercise both group dependency rules: a Pass Through child adjustment
    // extends the parent accumulator dependency, while an Isolated branch
    // contributes its own independent maximum before an outer adjustment.
    LayerNode passAdjustment;
    passAdjustment.type = LayerType::Adjustment;
    ShadowsHighlightsParameters passParameters = parameters;
    passParameters.radius = 41.0;
    passAdjustment.setShadowsHighlightsParameters(passParameters);
    LayerNode passGroup;
    passGroup.type = LayerType::Group;
    passGroup.groupCompositeMode = GroupCompositeMode::PassThrough;
    passGroup.children = {passAdjustment};

    LayerNode isolatedRaster;
    isolatedRaster.type = LayerType::Raster;
    isolatedRaster.rasterImage = source;
    isolatedRaster.rasterReferenceSize = size;
    isolatedRaster.opacity = 0.43;
    isolatedRaster.blendMode = BlendMode::Screen;
    LayerNode isolatedAdjustment;
    isolatedAdjustment.type = LayerType::Adjustment;
    ShadowsHighlightsParameters isolatedParameters = parameters;
    isolatedParameters.radius = 67.0;
    isolatedAdjustment.setShadowsHighlightsParameters(isolatedParameters);
    LayerNode isolatedGroup;
    isolatedGroup.type = LayerType::Group;
    isolatedGroup.groupCompositeMode = GroupCompositeMode::Isolated;
    isolatedGroup.opacity = 0.71;
    isolatedGroup.children = {isolatedAdjustment, isolatedRaster};

    LayerNode outerAdjustment;
    outerAdjustment.type = LayerType::Adjustment;
    ShadowsHighlightsParameters outerParameters = upperParameters;
    outerParameters.radius = 23.0;
    outerAdjustment.setShadowsHighlightsParameters(outerParameters);
    const QVector<LayerNode> groupedLayers {
        outerAdjustment, isolatedGroup, passGroup, base
    };
    QCOMPARE(maximumSpatialAdjustmentRadius(groupedLayers), 90);
    const QImage groupedFull = ImageProcessor::renderRegion(
        source, groupedLayers, source.rect(), size);
    const QRect groupedRegion(223, 231, 181, 153);
    const QImage groupedTile = ImageProcessor::renderRegion(
        source, groupedLayers, groupedRegion, size);
    QVERIFY(!groupedFull.isNull());
    QVERIFY(!groupedTile.isNull());
    QVERIFY(exactImagesEqual(groupedTile, groupedFull.copy(groupedRegion)));
}

void CoreTests::histogramCapturesExactEightAndSixteenBitInput()
{
    QImage source8(3, 1, QImage::Format_RGBA8888);
    source8.setPixelColor(0, 0, QColor(10, 20, 30, 255));
    source8.setPixelColor(1, 0, QColor(10, 40, 50, 128));
    source8.setPixelColor(2, 0, QColor(200, 201, 202, 0));
    LayerNode base8;
    base8.type = LayerType::BaseImage;
    base8.rasterImage = source8;
    base8.rasterReferenceSize = source8.size();
    LayerNode levels8;
    levels8.type = LayerType::Adjustment;
    levels8.resetAdjustmentParameters(AdjustmentType::Levels);

    HistogramRequest request8;
    request8.documentSessionId = QUuid::createUuid();
    request8.adjustmentLayerId = levels8.id;
    request8.documentRevision = 1;
    request8.source = source8;
    request8.layers = {levels8, base8};
    request8.documentSize = source8.size();
    const HistogramData histogram8 = HistogramService::calculate(request8);
    QVERIFY(histogram8.isValid());
    QCOMPARE(histogram8.binCount, 256);
    QCOMPARE(histogram8.includedPixels, quint64(2));
    QCOMPARE(histogram8.transparentPixels, quint64(1));
    QCOMPARE(histogram8.red.at(10), quint64(510));
    QCOMPARE(histogram8.alpha.at(255), quint64(255));
    QCOMPARE(histogram8.alpha.at(128), quint64(255));
    QCOMPARE(histogram8.alpha.at(0), quint64(255));
    QCOMPARE(histogram8.red.at(200), quint64(0));

    QImage source16(2, 1, QImage::Format_RGBA64);
    auto *pixels = reinterpret_cast<QRgba64 *>(source16.scanLine(0));
    pixels[0] = QRgba64::fromRgba64(12345, 23456, 34567, 65535);
    pixels[1] = QRgba64::fromRgba64(54321, 43210, 32109, 32768);
    LayerNode base16 = base8;
    base16.id = QUuid::createUuid();
    base16.rasterImage = source16;
    base16.rasterReferenceSize = source16.size();
    LayerNode levels16 = levels8;
    levels16.id = QUuid::createUuid();
    HistogramRequest request16;
    request16.documentSessionId = QUuid::createUuid();
    request16.adjustmentLayerId = levels16.id;
    request16.documentRevision = 1;
    request16.source = source16;
    request16.layers = {levels16, base16};
    request16.documentSize = source16.size();
    const HistogramData histogram16 = HistogramService::calculate(request16);
    QVERIFY(histogram16.isValid());
    QCOMPARE(histogram16.binCount, 65536);
    QCOMPARE(histogram16.sourceBitDepth, 16);
    QCOMPARE(histogram16.red.at(12345), quint64(255));
    QCOMPARE(histogram16.red.at(54321), quint64(255));
    QCOMPARE(histogram16.alpha.at(65535), quint64(255));
    QCOMPARE(histogram16.alpha.at(32768), quint64(255));
}

void CoreTests::parallelHistogramReductionIsExactAndDeterministic()
{
    // Large enough to cross the private parallel threshold while retaining a
    // closed-form expected histogram for every component and Alpha value.
    const QSize size(1280, 820);
    QImage source(size, QImage::Format_RGBA8888);
    std::array<quint64, 256> expectedRed {};
    std::array<quint64, 256> expectedGreen {};
    std::array<quint64, 256> expectedBlue {};
    std::array<quint64, 256> expectedAlpha {};
    quint64 expectedIncluded = 0;
    quint64 expectedTransparent = 0;
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int red = (x * 7 + y * 3) & 255;
            const int green = (x * 5 + y * 11) & 255;
            const int blue = (x * 13 + y * 2) & 255;
            const int alpha = ((x + y) % 97 == 0) ? 0 : ((x * 17 + y) & 255);
            uchar *pixel = row + x * 4;
            pixel[0] = static_cast<uchar>(red);
            pixel[1] = static_cast<uchar>(green);
            pixel[2] = static_cast<uchar>(blue);
            pixel[3] = static_cast<uchar>(alpha);
            expectedAlpha[static_cast<std::size_t>(alpha)] += 255;
            if (alpha == 0) {
                ++expectedTransparent;
            } else {
                ++expectedIncluded;
                expectedRed[static_cast<std::size_t>(red)] += 255;
                expectedGreen[static_cast<std::size_t>(green)] += 255;
                expectedBlue[static_cast<std::size_t>(blue)] += 255;
            }
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = size;
    LayerNode curves;
    curves.type = LayerType::Adjustment;
    curves.resetAdjustmentParameters(AdjustmentType::Curves);

    HistogramRequest request;
    request.documentSessionId = QUuid::createUuid();
    request.adjustmentLayerId = curves.id;
    request.documentRevision = 47;
    request.source = source;
    request.layers = {curves, base};
    request.documentSize = size;

    const HistogramData first = HistogramService::calculate(request);
    const HistogramData second = HistogramService::calculate(request);
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QCOMPARE(first.includedPixels, expectedIncluded);
    QCOMPARE(first.transparentPixels, expectedTransparent);
    QCOMPARE(first.luminance, second.luminance);
    QCOMPARE(first.red, second.red);
    QCOMPARE(first.green, second.green);
    QCOMPARE(first.blue, second.blue);
    QCOMPARE(first.alpha, second.alpha);
    for (int value = 0; value < 256; ++value) {
        QCOMPARE(first.red[value], expectedRed[static_cast<std::size_t>(value)]);
        QCOMPARE(first.green[value], expectedGreen[static_cast<std::size_t>(value)]);
        QCOMPARE(first.blue[value], expectedBlue[static_cast<std::size_t>(value)]);
        QCOMPARE(first.alpha[value], expectedAlpha[static_cast<std::size_t>(value)]);
    }
}

void CoreTests::histogramSelectionScopeUsesSparseCoverageAndCancellation()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(10, 20, 30, 255));
    source.setPixelColor(1, 0, QColor(200, 210, 220, 255));

    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();
    LayerNode levels;
    levels.type = LayerType::Adjustment;
    levels.resetAdjustmentParameters(AdjustmentType::Levels);

    SelectionMask selection(source.size());
    selection.selectNone();
    QVERIFY(selection.setCoverageRect(QRect(1, 0, 1, 1), 128));

    HistogramRequest request;
    request.documentSessionId = QUuid::createUuid();
    request.adjustmentLayerId = levels.id;
    request.documentRevision = 1;
    request.source = source;
    request.layers = {levels, base};
    request.documentSize = source.size();
    request.scope = HistogramScope::Selection;
    request.selection = selection.snapshot();

    const HistogramData selected = HistogramService::calculate(request);
    QVERIFY(selected.isValid());
    QCOMPARE(selected.includedPixels, quint64(1));
    QCOMPARE(selected.includedWeight, quint64(128));
    QCOMPARE(selected.red.at(10), quint64(0));
    QCOMPARE(selected.red.at(200), quint64(128));

    std::atomic_bool cancelled {true};
    const HistogramData cancelledResult = HistogramService::calculate(request, &cancelled);
    QVERIFY(cancelledResult.cancelled);
    QVERIFY(!cancelledResult.isValid());
}

void CoreTests::adjustmentInputHistogramRespectsGroupBoundaries()
{
    QImage source(1, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(10, 0, 0, 255));
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();

    LayerNode child;
    child.type = LayerType::Raster;
    child.rasterImage = QImage(1, 1, QImage::Format_RGBA8888);
    child.rasterImage.setPixelColor(0, 0, QColor(200, 0, 0, 128));
    child.rasterReferenceSize = source.size();

    LayerNode target;
    target.type = LayerType::Adjustment;
    target.resetAdjustmentParameters(AdjustmentType::Levels);

    LayerNode group;
    group.type = LayerType::Group;
    group.children = {target, child};

    group.groupCompositeMode = GroupCompositeMode::Isolated;
    const QImage isolated = ImageProcessor::renderAdjustmentInput(
        source, {group, base}, target.id, source.size());
    QVERIFY(!isolated.isNull());
    const QColor isolatedPixel = isolated.pixelColor(0, 0);
    QCOMPARE(isolatedPixel.red(), 200);
    QCOMPARE(isolatedPixel.alpha(), 128);

    group.groupCompositeMode = GroupCompositeMode::PassThrough;
    const QImage passThrough = ImageProcessor::renderAdjustmentInput(
        source, {group, base}, target.id, source.size());
    QVERIFY(!passThrough.isNull());
    const QColor passThroughPixel = passThrough.pixelColor(0, 0);
    QVERIFY(passThroughPixel.red() >= 104 && passThroughPixel.red() <= 106);
    QCOMPARE(passThroughPixel.alpha(), 255);
}


void CoreTests::selectiveColourAdjustmentsRoundTripSchemaThree()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage source(3, 2, QImage::Format_RGBA64);
    source.fill(QColor(91, 132, 204, 177));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("selective-colour.png"));

    const QUuid hueId = document.addAdjustment(AdjustmentType::HueSaturation);
    const QUuid vibranceId = document.addAdjustment(AdjustmentType::Vibrance);
    const QUuid whiteId = document.addAdjustment(AdjustmentType::WhiteBalance);
    const QUuid balanceId = document.addAdjustment(AdjustmentType::ColourBalance);
    QVERIFY(!hueId.isNull() && !vibranceId.isNull()
            && !whiteId.isNull() && !balanceId.isNull());

    HueSaturationParameters hue;
    hue.hue = 17.0;
    hue.saturation = 21.0;
    hue.lightness = -8.0;
    auto &reds = hue.range(HueSaturationRange::Reds);
    reds.hue = -23.0;
    reds.saturation = 44.0;
    reds.lightness = 9.0;
    reds.centre = 7.0;
    reds.width = 42.0;
    reds.feather = 19.0;
    hue.normalise();
    VibranceParameters vibrance {62.0, -13.0, 81.0};
    vibrance.normalise();
    WhiteBalanceParameters white {31.0, -22.0};
    white.normalise();
    ColourBalanceParameters balance;
    balance.range(ColourBalanceRange::Shadows) = {-18.0, 11.0, 7.0};
    balance.range(ColourBalanceRange::Midtones) = {9.0, -16.0, 24.0};
    balance.range(ColourBalanceRange::Highlights) = {21.0, 8.0, -14.0};
    balance.preserveLuminosity = false;
    balance.normalise();

    QVERIFY(document.updateLayer(hueId, [hue](LayerNode &layer) { layer.setHueSaturationParameters(hue); }));
    QVERIFY(document.updateLayer(vibranceId, [vibrance](LayerNode &layer) { layer.setVibranceParameters(vibrance); }));
    QVERIFY(document.updateLayer(whiteId, [white](LayerNode &layer) { layer.setWhiteBalanceParameters(white); }));
    QVERIFY(document.updateLayer(balanceId, [balance](LayerNode &layer) { layer.setColourBalanceParameters(balance); }));

    const QString path = directory.filePath(QStringLiteral("selective-colour.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);
    const QJsonArray layers = json.object().value(QStringLiteral("layers")).toArray();
    int typedCount = 0;
    for (const QJsonValue &value : layers) {
        const QJsonObject adjustment = value.toObject().value(QStringLiteral("adjustment")).toObject();
        if (!adjustment.isEmpty()) {
            QCOMPARE(adjustment.value(QStringLiteral("schema")).toInt(),
                     static_cast<int>(AdjustmentData::CurrentSchema));
            ++typedCount;
        }
    }
    QCOMPARE(typedCount, 4);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(std::get<HueSaturationParameters>(restored.layerById(hueId).effectiveAdjustmentData().parameters) == hue);
    QVERIFY(std::get<VibranceParameters>(restored.layerById(vibranceId).effectiveAdjustmentData().parameters) == vibrance);
    QVERIFY(std::get<WhiteBalanceParameters>(restored.layerById(whiteId).effectiveAdjustmentData().parameters) == white);
    QVERIFY(std::get<ColourBalanceParameters>(restored.layerById(balanceId).effectiveAdjustmentData().parameters) == balance);
}

void CoreTests::selectiveColourIdentityPreservesEightAndSixteenBitHiddenRgb()
{
    const QVector<AdjustmentType> types {
        AdjustmentType::HueSaturation,
        AdjustmentType::Vibrance,
        AdjustmentType::WhiteBalance,
        AdjustmentType::ColourBalance
    };
    for (const QImage::Format format : {QImage::Format_RGBA8888, QImage::Format_RGBA64}) {
        QImage source(2, 1, format);
        if (format == QImage::Format_RGBA64) {
            auto *pixels = reinterpret_cast<QRgba64 *>(source.bits());
            pixels[0] = QRgba64::fromRgba64(1234, 45678, 32100, 0);
            pixels[1] = QRgba64::fromRgba64(61000, 2000, 19000, 33000);
        } else {
            source.setPixelColor(0, 0, QColor(19, 173, 91, 0));
            source.setPixelColor(1, 0, QColor(231, 17, 88, 129));
        }
        LayerNode base;
        base.type = LayerType::BaseImage;
        base.rasterImage = source;
        base.rasterReferenceSize = source.size();
        for (const AdjustmentType type : types) {
            LayerNode adjustment;
            adjustment.type = LayerType::Adjustment;
            adjustment.resetAdjustmentParameters(type);
            const QImage result = ImageProcessor::renderPreservingHiddenRgb(source, {adjustment, base});
            QVERIFY2(exactImagesEqual(result.convertToFormat(format), source),
                     qPrintable(defaultAdjustmentName(type)));
        }
    }
}

void CoreTests::targetedHueRangeAndVibranceRemainSelective()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(220, 35, 32, 255));
    source.setPixelColor(1, 0, QColor(20, 190, 205, 255));
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();

    LayerNode hueLayer;
    hueLayer.type = LayerType::Adjustment;
    HueSaturationParameters hue;
    hue.range(HueSaturationRange::Reds).hue = 80.0;
    hue.range(HueSaturationRange::Reds).width = 42.0;
    hue.range(HueSaturationRange::Reds).feather = 12.0;
    hueLayer.setHueSaturationParameters(hue);
    const QImage shifted = ImageProcessor::render(source, {hueLayer, base}).convertToFormat(QImage::Format_RGBA8888);
    const QColor shiftedRed = shifted.pixelColor(0, 0);
    const QColor shiftedCyan = shifted.pixelColor(1, 0);
    QVERIFY(std::abs(shiftedRed.red() - 220) > 20 || std::abs(shiftedRed.green() - 35) > 20);
    QVERIFY(std::abs(shiftedCyan.red() - 20) <= 2);
    QVERIFY(std::abs(shiftedCyan.green() - 190) <= 2);
    QVERIFY(std::abs(shiftedCyan.blue() - 205) <= 2);

    LayerNode vibranceLayer;
    vibranceLayer.type = LayerType::Adjustment;
    VibranceParameters vibrance;
    vibrance.vibrance = 80.0;
    vibrance.skinProtection = 100.0;
    vibranceLayer.setVibranceParameters(vibrance);
    const QImage vibrant = ImageProcessor::render(source, {vibranceLayer, base}).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(vibrant.pixelColor(1, 0) != source.pixelColor(1, 0));
}

void CoreTests::whiteAndColourBalancePreserveAlphaAndNeutralLuminosity()
{
    QImage source(1, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(111, 127, 143, 73));
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();

    LayerNode white;
    white.type = LayerType::Adjustment;
    white.setWhiteBalanceParameters({45.0, -30.0});
    const QColor whitePixel = ImageProcessor::render(source, {white, base})
        .convertToFormat(QImage::Format_RGBA8888).pixelColor(0, 0);
    QCOMPARE(whitePixel.alpha(), 73);
    QVERIFY(whitePixel != source.pixelColor(0, 0));

    LayerNode balance;
    balance.type = LayerType::Adjustment;
    ColourBalanceParameters parameters;
    parameters.range(ColourBalanceRange::Midtones).cyanRed = 65.0;
    parameters.range(ColourBalanceRange::Midtones).yellowBlue = -45.0;
    parameters.preserveLuminosity = true;
    balance.setColourBalanceParameters(parameters);
    const QColor balanced = ImageProcessor::render(source, {balance, base})
        .convertToFormat(QImage::Format_RGBA8888).pixelColor(0, 0);
    QCOMPARE(balanced.alpha(), 73);
    const auto luminance = [](const QColor &colour) {
        return 0.2126 * colour.redF() + 0.7152 * colour.greenF() + 0.0722 * colour.blueF();
    };
    QVERIFY(std::abs(luminance(balanced) - luminance(source.pixelColor(0, 0))) < 0.08);
}


void CoreTests::channelAndTonalAdjustmentsRoundTripSchemaFive()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage source(4, 3, QImage::Format_RGBA64);
    source.fill(QColor(73, 119, 201, 143));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("channel-tonal.png"));

    const QUuid mixerId = document.addAdjustment(AdjustmentType::ChannelMixer);
    const QUuid blackWhiteId = document.addAdjustment(AdjustmentType::BlackAndWhite);
    const QUuid gradientId = document.addAdjustment(AdjustmentType::GradientMap);
    QVERIFY(!mixerId.isNull() && !blackWhiteId.isNull() && !gradientId.isNull());

    ChannelMixerParameters mixer;
    mixer.output(ChannelMixerOutput::Red) = {112.0, -9.0, 3.0, -4.0};
    mixer.output(ChannelMixerOutput::Green) = {-7.0, 108.0, 5.0, 2.0};
    mixer.output(ChannelMixerOutput::Blue) = {4.0, 12.0, 93.0, 1.0};
    mixer.monochrome = {31.0, 52.0, 17.0, 3.0};
    mixer.monochromeEnabled = false;
    mixer.normalise();

    BlackAndWhiteParameters blackWhite;
    blackWhite.colourWeights = {147.0, 81.0, 116.0, 64.0, 129.0, 92.0};
    blackWhite.tintEnabled = true;
    blackWhite.tintHue = 42.0;
    blackWhite.tintSaturation = 28.0;
    blackWhite.normalise();

    GradientMapParameters gradient;
    gradient.stops = {{0.0, QColor(4, 8, 22)},
                      {0.33, QColor(71, 28, 106)},
                      {0.68, QColor(232, 118, 54)},
                      {1.0, QColor(255, 247, 214)}};
    gradient.reverse = true;
    gradient.interpolation = GradientInterpolation::Smooth;
    gradient.normalise();

    QVERIFY(document.updateLayer(mixerId, [mixer](LayerNode &layer) {
        layer.setChannelMixerParameters(mixer);
    }));
    QVERIFY(document.updateLayer(blackWhiteId, [blackWhite](LayerNode &layer) {
        layer.setBlackAndWhiteParameters(blackWhite);
    }));
    QVERIFY(document.updateLayer(gradientId, [gradient](LayerNode &layer) {
        layer.setGradientMapParameters(gradient);
    }));

    const QString path = directory.filePath(QStringLiteral("channel-tonal.vfxphoto"));
    QString error;
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    QCOMPARE(json.object().value(QStringLiteral("version")).toInt(), PhotoDocument::ProjectFormatVersion);
    const QJsonArray layers = json.object().value(QStringLiteral("layers")).toArray();
    int currentSchemaCount = 0;
    for (const QJsonValue &encoded : layers) {
        const QJsonObject adjustment = encoded.toObject().value(QStringLiteral("adjustment")).toObject();
        if (adjustment.value(QStringLiteral("schema")).toInt()
            == static_cast<int>(AdjustmentData::CurrentSchema)) {
            ++currentSchemaCount;
        }
    }
    QCOMPARE(currentSchemaCount, 3);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QVERIFY(std::get<ChannelMixerParameters>(
        restored.layerById(mixerId).effectiveAdjustmentData().parameters) == mixer);
    QVERIFY(std::get<BlackAndWhiteParameters>(
        restored.layerById(blackWhiteId).effectiveAdjustmentData().parameters) == blackWhite);
    QVERIFY(std::get<GradientMapParameters>(
        restored.layerById(gradientId).effectiveAdjustmentData().parameters) == gradient);
}

void CoreTests::channelMixerIdentityMonochromeAndHiddenRgb()
{
    for (const QImage::Format format : {QImage::Format_RGBA8888, QImage::Format_RGBA64}) {
        QImage source(2, 1, format);
        if (format == QImage::Format_RGBA64) {
            auto *pixels = reinterpret_cast<QRgba64 *>(source.bits());
            pixels[0] = QRgba64::fromRgba64(5111, 32123, 61234, 0);
            pixels[1] = QRgba64::fromRgba64(50000, 12000, 30000, 39000);
        } else {
            source.setPixelColor(0, 0, QColor(17, 139, 231, 0));
            source.setPixelColor(1, 0, QColor(211, 51, 113, 151));
        }
        LayerNode base;
        base.type = LayerType::BaseImage;
        base.rasterImage = source;
        base.rasterReferenceSize = source.size();
        LayerNode mixer;
        mixer.type = LayerType::Adjustment;
        mixer.resetAdjustmentParameters(AdjustmentType::ChannelMixer);
        const QImage identity = ImageProcessor::renderPreservingHiddenRgb(source, {mixer, base});
        QVERIFY(exactImagesEqual(identity.convertToFormat(format), source));

        ChannelMixerParameters parameters;
        parameters.monochromeEnabled = true;
        parameters.monochrome = {30.0, 59.0, 11.0, 0.0};
        mixer.setChannelMixerParameters(parameters);
        const QImage monochrome = ImageProcessor::renderPreservingHiddenRgb(source, {mixer, base})
            .convertToFormat(format);
        if (format == QImage::Format_RGBA64) {
            const auto *pixels = reinterpret_cast<const QRgba64 *>(monochrome.constBits());
            QCOMPARE(pixels[0].red(), pixels[0].green());
            QCOMPARE(pixels[0].green(), pixels[0].blue());
            QCOMPARE(pixels[0].alpha(), quint16(0));
        } else {
            const QColor pixel = monochrome.pixelColor(0, 0);
            QCOMPARE(pixel.red(), pixel.green());
            QCOMPARE(pixel.green(), pixel.blue());
            QCOMPARE(pixel.alpha(), 0);
        }
    }
}

void CoreTests::blackAndWhiteTargetsColourFamiliesAndPreservesAlpha()
{
    QImage source(3, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(230, 25, 20, 73));
    source.setPixelColor(1, 0, QColor(20, 215, 32, 113));
    source.setPixelColor(2, 0, QColor(18, 42, 228, 191));
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();

    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    BlackAndWhiteParameters parameters;
    parameters.colourWeights[0] = 220.0;
    parameters.colourWeights[2] = 35.0;
    parameters.colourWeights[4] = 90.0;
    adjustment.setBlackAndWhiteParameters(parameters);
    const QImage result = ImageProcessor::renderPreservingHiddenRgb(source, {adjustment, base})
        .convertToFormat(QImage::Format_RGBA8888);
    for (int x = 0; x < 3; ++x) {
        const QColor pixel = result.pixelColor(x, 0);
        QCOMPARE(pixel.red(), pixel.green());
        QCOMPARE(pixel.green(), pixel.blue());
        QCOMPARE(pixel.alpha(), source.pixelColor(x, 0).alpha());
    }
    QVERIFY(result.pixelColor(0, 0).red() > result.pixelColor(1, 0).red());

    parameters.tintEnabled = true;
    parameters.tintHue = 210.0;
    parameters.tintSaturation = 45.0;
    adjustment.setBlackAndWhiteParameters(parameters);
    const QColor tinted = ImageProcessor::renderPreservingHiddenRgb(source, {adjustment, base})
        .convertToFormat(QImage::Format_RGBA8888).pixelColor(0, 0);
    QVERIFY(tinted.blue() > tinted.red());
    QCOMPARE(tinted.alpha(), 73);
}

void CoreTests::gradientMapUsesExactLuminanceLookupAtEightAndSixteenBit()
{
    GradientMapParameters parameters;
    parameters.stops = {{0.0, QColor(0, 0, 0)},
                        {0.5, QColor(255, 0, 0)},
                        {1.0, QColor(255, 255, 255)}};
    parameters.interpolation = GradientInterpolation::Linear;
    AdjustmentData data;
    data.reset(AdjustmentType::GradientMap);
    data.parameters = parameters;
    const TonalLookupTable lookup8 = buildTonalLookup(data, 8);
    const TonalLookupTable lookup16 = buildTonalLookup(data, 16);
    QVERIFY(lookup8.isValid());
    QVERIFY(lookup16.isValid());
    QCOMPARE(lookup8.maximumValue, 255);
    QCOMPARE(lookup16.maximumValue, 65535);
    QVERIFY(lookup16.map(0, 16384) > lookup8.map(0, 64) * 256 - 300);
    QVERIFY(lookup16.map(1, 49152) > 0);

    GradientMapParameters constantParameters;
    constantParameters.stops = {{0.0, QColor(0, 0, 0)},
                                {128.0 / 255.0, QColor(255, 0, 0)},
                                {1.0, QColor(255, 255, 255)}};
    constantParameters.interpolation = GradientInterpolation::Constant;
    data.parameters = constantParameters;
    const TonalLookupTable constantLookup = buildTonalLookup(data, 8);
    QCOMPARE(constantLookup.map(0, 127), quint16(0));
    QCOMPARE(constantLookup.map(0, 128), quint16(255));
    QCOMPARE(constantLookup.map(1, 128), quint16(0));

    QImage source(2, 1, QImage::Format_RGBA64);
    auto *pixels = reinterpret_cast<QRgba64 *>(source.bits());
    pixels[0] = QRgba64::fromRgba64(10000, 30000, 50000, 0);
    pixels[1] = QRgba64::fromRgba64(50000, 30000, 10000, 42000);
    LayerNode base;
    base.type = LayerType::BaseImage;
    base.rasterImage = source;
    base.rasterReferenceSize = source.size();
    LayerNode gradient;
    gradient.type = LayerType::Adjustment;
    gradient.setGradientMapParameters(parameters);
    const QImage result = ImageProcessor::renderPreservingHiddenRgb(source, {gradient, base})
        .convertToFormat(QImage::Format_RGBA64);
    const auto *output = reinterpret_cast<const QRgba64 *>(result.constBits());
    QCOMPARE(output[0].alpha(), quint16(0));
    QCOMPARE(output[1].alpha(), quint16(42000));
    QVERIFY(output[0].red() != pixels[0].red() || output[0].green() != pixels[0].green());

    parameters.reverse = true;
    gradient.setGradientMapParameters(parameters);
    const QImage reversed = ImageProcessor::renderPreservingHiddenRgb(source, {gradient, base})
        .convertToFormat(QImage::Format_RGBA64);
    QVERIFY(!exactImagesEqual(result, reversed));
}


void CoreTests::vectorShapeLayerRoundTripsVersionSevenAndRejectsPreVersionSeven()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Vector Foundation");
    settings.pixelSize = QSize(96, 72);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(12, 23, 34, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const QColor fill = QColor::fromRgba64(
        QRgba64::fromRgba64(12345, 34567, 54321, 45678));
    const QUuid vectorId = document.addVectorShape(
        VectorShapeType::RoundedRectangle,
        QRectF(-12.5, 7.25, 44.5, 28.75), fill, {}, 6.5);
    QVERIFY(!vectorId.isNull());
    QVERIFY(document.addMask(vectorId));
    QVERIFY(document.updateLayer(vectorId, [](LayerNode &layer) {
        layer.opacity = 0.81;
        layer.blendMode = BlendMode::Multiply;
        layer.vectorData.featherRadius = 13.5;
        layer.transform = QTransform::fromTranslate(8.5, -3.25)
            * QTransform::fromScale(1.25, 0.75);
        VectorShape &shape = layer.vectorData.objects.first();
        shape.fill.opacity = 0.67;
        shape.cornerRadii = VectorCornerRadii {3.0, 6.0, 9.0, 12.0};
        shape.cornerRadiiLinked = false;
        shape.transform = QTransform::fromTranslate(2.0, 1.5);
        ++shape.revision;
        layer.vectorData.normalise();
    }));
    const LayerNode expected = document.layerById(vectorId);
    QVERIFY(expected.vectorData.isSafe());

    const QString path = directory.filePath(QStringLiteral("vector-v7.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QJsonDocument encoded = QJsonDocument::fromJson(saved.readAll());
    saved.close();
    QVERIFY(encoded.isObject());
    QCOMPARE(encoded.object().value(QStringLiteral("version")).toInt(),
             PhotoDocument::ProjectFormatVersion);

    bool foundVector = false;
    for (const QJsonValue &value : encoded.object()
             .value(QStringLiteral("layerTree")).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("kind")).toString() == QStringLiteral("vector")) {
            foundVector = true;
            const QJsonObject vectorObject = object.value(
                QStringLiteral("vector")).toObject();
            QCOMPARE(vectorObject.value(QStringLiteral("schema")).toInt(),
                     static_cast<int>(VectorLayerData::CurrentSchema));
            QCOMPARE(vectorObject.value(QStringLiteral("featherRadius")).toDouble(),
                     13.5);
        }
    }
    QVERIFY(foundVector);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    const LayerNode actual = restored.layerById(vectorId);
    QCOMPARE(actual.type, LayerType::Vector);
    QCOMPARE(actual.vectorData, expected.vectorData);
    QCOMPARE(actual.opacity, expected.opacity);
    QCOMPARE(actual.blendMode, expected.blendMode);
    QVERIFY(transformsClose(actual.transform, expected.transform));
    QVERIFY(actual.hasMask());

    QJsonObject compatibleV15 = encoded.object();
    compatibleV15.insert(QStringLiteral("version"), 15);
    QJsonArray compatibleV15Layers = compatibleV15
        .value(QStringLiteral("layerTree")).toArray();
    for (qsizetype index = 0; index < compatibleV15Layers.size(); ++index) {
        QJsonObject layerObject = compatibleV15Layers.at(index).toObject();
        if (layerObject.value(QStringLiteral("kind")).toString()
            != QStringLiteral("vector")) {
            continue;
        }
        QJsonObject vectorObject = layerObject.value(QStringLiteral("vector")).toObject();
        vectorObject.insert(QStringLiteral("schema"), 7);
        vectorObject.remove(QStringLiteral("featherRadius"));
        layerObject.insert(QStringLiteral("vector"), vectorObject);
        compatibleV15Layers.replace(index, layerObject);
    }
    compatibleV15.insert(QStringLiteral("layerTree"), compatibleV15Layers);
    const QString compatibleV15Path = directory.filePath(
        QStringLiteral("compatible-vector-v15.vfxphoto"));
    QFile compatibleV15File(compatibleV15Path);
    QVERIFY(compatibleV15File.open(QIODevice::WriteOnly));
    const QByteArray compatibleV15Bytes = QJsonDocument(compatibleV15)
        .toJson(QJsonDocument::Compact);
    QCOMPARE(compatibleV15File.write(compatibleV15Bytes),
             static_cast<qint64>(compatibleV15Bytes.size()));
    compatibleV15File.close();
    PhotoDocument migratedV15;
    QVERIFY2(migratedV15.loadProject(compatibleV15Path, &error), qPrintable(error));
    QCOMPARE(migratedV15.layerById(vectorId).vectorData.featherRadius, 0.0);

    QJsonObject dishonestV15 = encoded.object();
    dishonestV15.insert(QStringLiteral("version"), 15);
    const QString dishonestV15Path = directory.filePath(
        QStringLiteral("illegal-vector-feather-v15.vfxphoto"));
    QFile dishonestV15File(dishonestV15Path);
    QVERIFY(dishonestV15File.open(QIODevice::WriteOnly));
    const QByteArray dishonestV15Bytes = QJsonDocument(dishonestV15)
        .toJson(QJsonDocument::Compact);
    QCOMPARE(dishonestV15File.write(dishonestV15Bytes),
             static_cast<qint64>(dishonestV15Bytes.size()));
    dishonestV15File.close();
    PhotoDocument rejectedV15;
    QVERIFY(!rejectedV15.loadProject(dishonestV15Path, &error));
    QVERIFY(error.contains(QStringLiteral("version-16"), Qt::CaseInsensitive));
    QVERIFY(error.contains(QStringLiteral("Feather"), Qt::CaseInsensitive));

    QJsonObject preVersionSeven = encoded.object();
    preVersionSeven.insert(QStringLiteral("version"), 6);
    const QString rejectedPath = directory.filePath(QStringLiteral("illegal-vector-v6.vfxphoto"));
    QFile rejectedFile(rejectedPath);
    QVERIFY(rejectedFile.open(QIODevice::WriteOnly));
    const QByteArray rejectedBytes = QJsonDocument(preVersionSeven)
        .toJson(QJsonDocument::Compact);
    QCOMPARE(rejectedFile.write(rejectedBytes), static_cast<qint64>(rejectedBytes.size()));
    rejectedFile.close();
    PhotoDocument rejected;
    QVERIFY(!rejected.loadProject(rejectedPath, &error));
    QVERIFY(error.contains(QStringLiteral("version-7"), Qt::CaseInsensitive));

    QJsonArray rasterOnly;
    for (const QJsonValue &value : encoded.object()
             .value(QStringLiteral("layerTree")).toArray()) {
        if (value.toObject().value(QStringLiteral("kind")).toString()
            != QStringLiteral("vector")) {
            rasterOnly.append(value);
        }
    }
    QJsonObject compatibleV6 = encoded.object();
    compatibleV6.insert(QStringLiteral("version"), 6);
    compatibleV6.insert(QStringLiteral("layerTree"), rasterOnly);
    const QString compatiblePath = directory.filePath(QStringLiteral("compatible-v6.vfxphoto"));
    QFile compatibleFile(compatiblePath);
    QVERIFY(compatibleFile.open(QIODevice::WriteOnly));
    const QByteArray compatibleBytes = QJsonDocument(compatibleV6)
        .toJson(QJsonDocument::Compact);
    QCOMPARE(compatibleFile.write(compatibleBytes),
             static_cast<qint64>(compatibleBytes.size()));
    compatibleFile.close();
    PhotoDocument compatible;
    QVERIFY2(compatible.loadProject(compatiblePath, &error), qPrintable(error));
    QVERIFY(compatible.hasImage());

    VectorShape invalidType;
    invalidType.type = static_cast<VectorShapeType>(999);
    QVERIFY(!invalidType.isSafe());

    VectorFill invalidFill;
    invalidFill.opacity = 1.5;
    bool invalidFillOk = true;
    QVERIFY(invalidFill.toJson(&invalidFillOk).isEmpty());
    QVERIFY(!invalidFillOk);

    VectorLayerData oversized;
    oversized.objects.resize(VectorLayerData::MaximumObjectCount + 1);
    oversized.normalise();
    QCOMPARE(oversized.objects.size(), VectorLayerData::MaximumObjectCount + 1);
    QVERIFY(!oversized.isSafe());
    bool oversizedOk = true;
    QVERIFY(oversized.toJson(&oversizedOk).isEmpty());
    QVERIFY(!oversizedOk);

    const QUuid tinyId = document.addVectorShape(
        VectorShapeType::RoundedRectangle,
        QRectF(2.0, 3.0, 10.0, 8.0), QColor(Qt::red), {}, 16.0);
    QVERIFY(!tinyId.isNull());
    const VectorShape tiny = document.layerById(tinyId)
        .vectorData.objects.constFirst();
    QCOMPARE(tiny.cornerRadii.topLeft, 4.0);
    QVERIFY(tiny.cornerRadiiFitWorldTransform(
        document.layerWorldTransform(tinyId)));
}


void CoreTests::vectorFeatherDataModelDuplicationAndMergeRules()
{
    VectorLayerData data;
    QCOMPARE(data.featherRadius, 0.0);
    data.normalise();
    QVERIFY(data.isSafe());

    VectorShape shape;
    shape.type = VectorShapeType::Rectangle;
    shape.bounds = QRectF(4.0, 6.0, 30.0, 22.0);
    shape.fill.enabled = true;
    shape.fill.colour = QColor(40, 150, 220, 210);
    shape.stroke.enabled = true;
    shape.stroke.width = 3.0;
    shape.normalise();
    data.objects = {shape};
    data.normalise();
    const quint64 zeroFingerprint = data.fingerprint();
    data.featherRadius = 12.5;
    data.normalise();
    QVERIFY(data.isSafe());
    QVERIFY(data.fingerprint() != zeroFingerprint);

    bool jsonOk = false;
    const QJsonObject encoded = data.toJson(&jsonOk);
    QVERIFY(jsonOk);
    QCOMPARE(encoded.value(QStringLiteral("schema")).toInt(), 8);
    QCOMPARE(encoded.value(QStringLiteral("featherRadius")).toDouble(), 12.5);

    bool decodedOk = false;
    const VectorLayerData decoded = VectorLayerData::fromJson(encoded, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(decoded, data);

    QJsonObject legacy = encoded;
    legacy.insert(QStringLiteral("schema"), 7);
    legacy.remove(QStringLiteral("featherRadius"));
    const VectorLayerData migrated = VectorLayerData::fromJson(legacy, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(migrated.schema, VectorLayerData::CurrentSchema);
    QCOMPARE(migrated.featherRadius, 0.0);
    QCOMPARE(migrated.objects, data.objects);

    QJsonObject dishonestLegacy = encoded;
    dishonestLegacy.insert(QStringLiteral("schema"), 7);
    VectorLayerData::fromJson(dishonestLegacy, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject missingCurrent = encoded;
    missingCurrent.remove(QStringLiteral("featherRadius"));
    VectorLayerData::fromJson(missingCurrent, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject negative = encoded;
    negative.insert(QStringLiteral("featherRadius"), -0.1);
    VectorLayerData::fromJson(negative, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject oversized = encoded;
    oversized.insert(QStringLiteral("featherRadius"),
                     VectorLayerData::MaximumFeatherRadius + 0.1);
    VectorLayerData::fromJson(oversized, &decodedOk);
    QVERIFY(!decodedOk);

    NewDocumentSettings settings;
    settings.pixelSize = QSize(120, 90);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QUuid lowerId = document.addVectorShape(
        VectorShapeType::Rectangle, QRectF(8.0, 12.0, 44.0, 30.0), QColor(Qt::red));
    const QUuid upperId = document.addVectorShape(
        VectorShapeType::Ellipse, QRectF(34.0, 26.0, 48.0, 38.0), QColor(Qt::blue));
    QVERIFY(!lowerId.isNull());
    QVERIFY(!upperId.isNull());
    for (const QUuid id : {lowerId, upperId}) {
        QVERIFY(document.updateLayer(id, [](LayerNode &layer) {
            layer.vectorData.featherRadius = 6.5;
            layer.vectorData.normalise();
        }));
        QCOMPARE(document.layerById(id).vectorData.featherRadius, 6.5);
    }

    const LayerMergePlan plan = LayerMergeOperations::analyse(
        document, {upperId, lowerId}, &error);
    QVERIFY2(plan.isValid(), qPrintable(error));
    LayerNode merged;
    QVERIFY(!LayerMergeOperations::buildMergedLayer(
        document, plan, &merged, &error));
    QVERIFY(error.contains(QStringLiteral("Feather"), Qt::CaseInsensitive));
    QVERIFY(error.contains(QStringLiteral("combined-silhouette"), Qt::CaseInsensitive));

    for (const QUuid id : {lowerId, upperId}) {
        QVERIFY(document.updateLayer(id, [](LayerNode &layer) {
            layer.vectorData.featherRadius = 0.0;
            layer.vectorData.normalise();
        }));
    }
    QVERIFY2(LayerMergeOperations::buildMergedLayer(
                 document, plan, &merged, &error), qPrintable(error));
    QCOMPARE(merged.vectorData.featherRadius, 0.0);
    QVERIFY(merged.vectorData.isSafe());

    // Duplication/copy must continue preserving a non-zero independent
    // Feather value even though editable vector Merge rejects it.
    QVERIFY(document.updateLayer(upperId, [](LayerNode &layer) {
        layer.vectorData.featherRadius = 6.5;
        layer.vectorData.normalise();
    }));
    const LayerNode source = document.layerById(upperId);
    const QVector<QUuid> duplicates = document.duplicateLayers({upperId});
    QCOMPARE(duplicates.size(), 1);
    QCOMPARE(document.layerById(duplicates.constFirst()).vectorData.featherRadius,
             source.vectorData.featherRadius);

    const QUuid copyId = document.insertVectorLayerCopy(
        source, document.layerWorldTransform(upperId));
    QVERIFY(!copyId.isNull());
    QCOMPARE(document.layerById(copyId).vectorData.featherRadius,
             source.vectorData.featherRadius);

    VectorAppearance appearance = VectorAppearance::sensibleDefaults(
        QColor(Qt::green), QColor(Qt::black), false);
    LayerNode presetTarget = source;
    for (VectorShape &object : presetTarget.vectorData.objects) {
        appearance.applyTo(object);
    }
    presetTarget.vectorData.normalise();
    QCOMPARE(presetTarget.vectorData.featherRadius,
             source.vectorData.featherRadius);
    QVERIFY(presetTarget.vectorData.isSafe());
}


void CoreTests::vectorFeatherCpuReferenceSoftensOnlyCombinedCoverage()
{
    VectorRasterizer::clearCache();
    const QSize size(128, 128);
    const QRect fullRegion(QPoint(), size);
    const QColorSpace colourSpace(QColorSpace::SRgb);

    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = QStringLiteral("CPU Feather Reference");
    VectorShape rectangle;
    rectangle.type = VectorShapeType::Rectangle;
    rectangle.bounds = QRectF(40.0, 40.0, 48.0, 48.0);
    rectangle.fill.enabled = true;
    rectangle.fill.colour = QColor(220, 45, 35, 160);
    rectangle.fill.opacity = 0.75;
    rectangle.stroke.enabled = true;
    rectangle.stroke.colour = QColor(25, 70, 230, 210);
    rectangle.stroke.opacity = 0.8;
    rectangle.stroke.width = 8.0;
    rectangle.stroke.alignment = VectorStrokeAlignment::Inside;
    rectangle.normalise();
    layer.vectorData.objects = {rectangle};
    layer.vectorData.normalise();
    QVERIFY(layer.vectorData.isSafe());

    const QImage zero = VectorRasterizer::renderLayerRegion(
        layer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!zero.isNull());
    const QRectF semanticBounds = VectorRasterizer::contentBounds(layer);
    QCOMPARE(semanticBounds, rectangle.path().boundingRect());

    layer.vectorData.featherRadius = 8.0;
    layer.vectorData.normalise();
    const QImage feathered = VectorRasterizer::renderLayerRegion(
        layer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!feathered.isNull());
    const QRectF featherBounds = VectorRasterizer::contentBounds(layer);
    QCOMPARE(featherBounds, semanticBounds.adjusted(-8.0, -8.0, 8.0, 8.0));

    // The original editable appearance remains exact well inside the silhouette.
    const QColor zeroInterior = zero.pixelColor(64, 64);
    const QColor featherInterior = feathered.pixelColor(64, 64);
    QCOMPARE(featherInterior.red(), zeroInterior.red());
    QCOMPARE(featherInterior.green(), zeroInterior.green());
    QCOMPARE(featherInterior.blue(), zeroInterior.blue());
    QVERIFY(std::abs(featherInterior.alpha() - zeroInterior.alpha()) <= 1);

    // The outside halo takes the nearest authored edge colour instead of a
    // blurred red/blue mixture, while only coverage falls away.
    const QColor halo = feathered.pixelColor(36, 64);
    QVERIFY(halo.alpha() > 0);
    QVERIFY(halo.alpha() < feathered.pixelColor(44, 64).alpha());
    QVERIFY(halo.blue() > halo.red() * 2);
    QCOMPARE(feathered.pixelColor(24, 64).alpha(), 0);

    // Requested regions are independent: stitched CPU tiles must equal one full
    // reference render even though every tile resolves its own off-tile support.
    QImage stitched(size, QImage::Format_RGBA8888);
    stitched.fill(Qt::transparent);
    for (int y = 0; y < size.height(); y += 32) {
        for (int x = 0; x < size.width(); x += 32) {
            const QRect region(x, y,
                               std::min(32, size.width() - x),
                               std::min(32, size.height() - y));
            const QImage tile = VectorRasterizer::renderLayerRegion(
                layer, size, region, size, QTransform(),
                QImage::Format_RGBA8888, colourSpace);
            QVERIFY(!tile.isNull());
            for (int row = 0; row < tile.height(); ++row) {
                std::memcpy(stitched.scanLine(region.y() + row) + region.x() * 4,
                            tile.constScanLine(row),
                            static_cast<size_t>(tile.width()) * 4);
            }
        }
    }
    QVERIFY(exactImagesEqual(stitched, feathered));

    // Integer coverage is the same established deterministic three-box CPU
    // equation used by the shared spatial-filter reference.
    LayerNode opaqueLayer = layer;
    opaqueLayer.vectorData.featherRadius = 0.0;
    opaqueLayer.vectorData.objects.first().fill.colour = QColor(255, 255, 255, 255);
    opaqueLayer.vectorData.objects.first().fill.opacity = 1.0;
    opaqueLayer.vectorData.objects.first().stroke.enabled = false;
    opaqueLayer.vectorData.normalise();
    const QImage opaqueZero = VectorRasterizer::renderLayerRegion(
        opaqueLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!opaqueZero.isNull());
    opaqueLayer.vectorData.featherRadius = 8.0;
    const QImage opaqueFeather = VectorRasterizer::renderLayerRegion(
        opaqueLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    const QImage expectedCoverage = SpatialFilterFoundation::gaussianBlurReference(
        opaqueZero, QSize(8, 8), SpatialEdgeMode::Transparent,
        SpatialAlphaMode::StraightRgba);
    QVERIFY(!opaqueFeather.isNull());
    QVERIFY(!expectedCoverage.isNull());
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            QVERIFY(std::abs(opaqueFeather.pixelColor(x, y).alpha()
                             - expectedCoverage.pixelColor(x, y).alpha()) <= 1);
        }
    }

    const QImage feathered64 = VectorRasterizer::renderLayerRegion(
        layer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA64, colourSpace);
    QVERIFY(!feathered64.isNull());
    const QRgba64 halo64 = reinterpret_cast<const QRgba64 *>(
        feathered64.constScanLine(64))[36];
    QVERIFY(halo64.alpha() > 0);
    QVERIFY(std::abs(halo64.alpha() / 65535.0 - halo.alphaF()) < 0.02);
    QVERIFY(halo64.blue() > halo64.red() * 2);

    // Alpha-safe channel presentation can reveal authored RGB even when the
    // ordinary composite sees a fully transparent vector colour.
    LayerNode hiddenRgbLayer = opaqueLayer;
    hiddenRgbLayer.vectorData.objects.first().fill.colour = QColor(35, 210, 145, 0);
    hiddenRgbLayer.vectorData.featherRadius = 4.0;
    hiddenRgbLayer.vectorData.normalise();
    const QImage hiddenNormal = VectorRasterizer::renderLayerRegion(
        hiddenRgbLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace, false);
    const QImage hiddenOpaque = VectorRasterizer::renderLayerRegion(
        hiddenRgbLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace, true);
    QVERIFY(!hiddenNormal.isNull());
    QVERIFY(!hiddenOpaque.isNull());
    QCOMPARE(hiddenNormal.pixelColor(64, 64).alpha(), 0);
    const QColor revealedInterior = hiddenOpaque.pixelColor(64, 64);
    QCOMPARE(revealedInterior.red(), 35);
    QCOMPARE(revealedInterior.green(), 210);
    QCOMPARE(revealedInterior.blue(), 145);
    QCOMPARE(revealedInterior.alpha(), 255);
    const QColor revealedHalo = hiddenOpaque.pixelColor(38, 64);
    QVERIFY(revealedHalo.alpha() > 0);
    QCOMPARE(revealedHalo.red(), 35);
    QCOMPARE(revealedHalo.green(), 210);
    QCOMPARE(revealedHalo.blue(), 145);

    // Even-odd holes feather inward but remain empty beyond the finite support.
    auto rectangularPath = [](const QRectF bounds) {
        VectorBezierPath path;
        path.closed = true;
        for (const QPointF point : {bounds.topLeft(), bounds.topRight(),
                                     bounds.bottomRight(), bounds.bottomLeft()}) {
            VectorPathNode node;
            node.anchor = point;
            node.clearHandles();
            path.nodes.push_back(node);
        }
        path.normalise();
        return path;
    };
    VectorShape ring;
    ring.type = VectorShapeType::Path;
    ring.bezierPath = rectangularPath(QRectF(18.0, 18.0, 92.0, 92.0));
    ring.additionalBezierPaths = {rectangularPath(QRectF(44.0, 44.0, 40.0, 40.0))};
    ring.pathFillRule = VectorPathFillRule::EvenOdd;
    ring.fill.enabled = true;
    ring.fill.colour = QColor(35, 190, 80, 190);
    ring.stroke.enabled = false;
    ring.normalise();
    LayerNode ringLayer;
    ringLayer.type = LayerType::Vector;
    ringLayer.vectorData.objects = {ring};
    ringLayer.vectorData.featherRadius = 6.0;
    ringLayer.vectorData.normalise();
    const QImage ringImage = VectorRasterizer::renderLayerRegion(
        ringLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!ringImage.isNull());
    QCOMPARE(ringImage.pixelColor(64, 64).alpha(), 0);
    QVERIFY(ringImage.pixelColor(46, 64).alpha() > 0);
    QVERIFY(ringImage.pixelColor(46, 64).alpha() < ringImage.pixelColor(34, 64).alpha());

    // Geometry can remain wholly outside the document while its coverage halo
    // reaches the requested image region.
    VectorShape outside;
    outside.type = VectorShapeType::Rectangle;
    outside.bounds = QRectF(-18.0, 48.0, 8.0, 24.0);
    outside.fill.enabled = true;
    outside.fill.colour = QColor(245, 170, 25, 255);
    outside.stroke.enabled = false;
    outside.normalise();
    LayerNode outsideLayer;
    outsideLayer.type = LayerType::Vector;
    outsideLayer.vectorData.objects = {outside};
    outsideLayer.vectorData.featherRadius = 16.0;
    outsideLayer.vectorData.normalise();
    const QImage outsideImage = VectorRasterizer::renderLayerRegion(
        outsideLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!outsideImage.isNull());
    QVERIFY(outsideImage.pixelColor(0, 60).alpha() > 0);
    QCOMPARE(outsideImage.pixelColor(20, 60).alpha(), 0);

    // Open paths and arrowheads remain part of the one editable stroke silhouette.
    VectorShape arrowLine;
    arrowLine.type = VectorShapeType::Line;
    arrowLine.lineStart = QPointF(24.0, 108.0);
    arrowLine.lineEnd = QPointF(100.0, 108.0);
    arrowLine.fill.enabled = false;
    arrowLine.stroke.enabled = true;
    arrowLine.stroke.colour = QColor(60, 205, 125, 220);
    arrowLine.stroke.width = 6.0;
    arrowLine.stroke.cap = VectorStrokeCap::Round;
    arrowLine.stroke.endArrowhead = VectorArrowheadType::Triangle;
    arrowLine.stroke.endArrowScale = 1.5;
    arrowLine.normalise();
    LayerNode arrowLayer;
    arrowLayer.type = LayerType::Vector;
    arrowLayer.vectorData.objects = {arrowLine};
    arrowLayer.vectorData.normalise();
    const QImage arrowZero = VectorRasterizer::renderLayerRegion(
        arrowLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!arrowZero.isNull());
    arrowLayer.vectorData.featherRadius = 5.0;
    const QImage arrowFeather = VectorRasterizer::renderLayerRegion(
        arrowLayer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!arrowFeather.isNull());
    QRect arrowZeroBounds;
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            if (arrowZero.pixelColor(x, y).alpha() <= 0) continue;
            const QRect pixelRect(x, y, 1, 1);
            arrowZeroBounds = arrowZeroBounds.isNull()
                ? pixelRect : arrowZeroBounds.united(pixelRect);
        }
    }
    QVERIFY(!arrowZeroBounds.isEmpty());
    bool arrowHaloFound = false;
    for (int y = 0; y < size.height() && !arrowHaloFound; ++y) {
        for (int x = 0; x < size.width(); ++x) {
            if (!arrowZeroBounds.contains(x, y)
                && arrowFeather.pixelColor(x, y).alpha() > 0) {
                arrowHaloFound = true;
                break;
            }
        }
    }
    QVERIFY(arrowHaloFound);
    const QColor arrowShaftZero = arrowZero.pixelColor(60, 108);
    const QColor arrowShaftFeather = arrowFeather.pixelColor(60, 108);
    QCOMPARE(arrowShaftFeather.red(), arrowShaftZero.red());
    QCOMPARE(arrowShaftFeather.green(), arrowShaftZero.green());
    QCOMPARE(arrowShaftFeather.blue(), arrowShaftZero.blue());

    // Very large persisted radii are evaluated directly from compact geometry;
    // they are neither clamped nor expanded into an empty radius-sized image.
    outsideLayer.vectorData.featherRadius = 50'000.0;
    const QRect tinyRequest(0, 56, 8, 8);
    const QImage hugeRadius = VectorRasterizer::renderLayerRegion(
        outsideLayer, size, tinyRequest, size, QTransform(),
        QImage::Format_RGBA64, colourSpace);
    QVERIFY(!hugeRadius.isNull());
    QCOMPARE(outsideLayer.vectorData.featherRadius, 50'000.0);

    // Fractional typed values interpolate between adjacent exact box supports
    // instead of being rounded or mutating the persisted property.
    layer.vectorData.featherRadius = 0.25;
    const QImage fractional = VectorRasterizer::renderLayerRegion(
        layer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!fractional.isNull());
    QCOMPARE(layer.vectorData.featherRadius, 0.25);
    QVERIFY(fractional.pixelColor(39, 64).alpha() > zero.pixelColor(39, 64).alpha());

    std::atomic_bool cancelled {true};
    const QImage cancelledImage = VectorRasterizer::renderLayerRegion(
        layer, size, fullRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace, false, false, &cancelled);
    QVERIFY(cancelledImage.isNull());
    VectorRasterizer::clearCache();
}


void CoreTests::vectorFeatherGpuPreparationMatchesCpuAndLocalisesTileCache()
{
    VectorRasterizer::clearCache();
    const QSize previewSize(160, 128);
    const QRect tileRect(28, 20, 104, 88);
    const QColorSpace colourSpace(QColorSpace::SRgb);

    LayerNode layer;
    layer.id = QUuid::createUuid();
    layer.type = LayerType::Vector;
    layer.name = QStringLiteral("GPU Feather Contract");
    VectorShape shape;
    shape.type = VectorShapeType::Rectangle;
    shape.bounds = QRectF(48.0, 38.0, 58.0, 50.0);
    shape.fill.enabled = true;
    shape.fill.colour = QColor(220, 55, 40, 170);
    shape.fill.opacity = 0.8;
    shape.stroke.enabled = true;
    shape.stroke.colour = QColor(30, 90, 225, 215);
    shape.stroke.opacity = 0.75;
    shape.stroke.width = 10.0;
    shape.stroke.alignment = VectorStrokeAlignment::Inside;
    shape.normalise();
    layer.vectorData.objects = {shape};
    layer.vectorData.featherRadius = 7.0;
    layer.vectorData.normalise();
    QVERIFY(layer.vectorData.isSafe());

    VectorFeatherGpuTileData prepared;
    QString error;
    QVERIFY2(VectorRasterizer::prepareGpuFeatherTile(
                 layer, previewSize, tileRect, previewSize, QTransform(),
                 colourSpace, false, false, &prepared, &error),
             qPrintable(error));
    QVERIFY(prepared.isValid());
    QCOMPARE(prepared.outputRect, tileRect);
    QCOMPARE(prepared.coverage.size(), prepared.sourceRect.size());
    QCOMPARE(prepared.colourCarrier.size(), tileRect.size());
    QCOMPARE(prepared.radiusX, 7.0);
    QCOMPARE(prepared.radiusY, 7.0);

    // Pathological direct preparation requests are rejected before allocating
    // an output-sized colour carrier. Normal compositor tiles are far below
    // this guard; this specifically protects the hard 256 MiB preparation
    // budget from unusual callers.
    VectorFeatherGpuTileData oversizedPrepared;
    QString oversizedError;
    QVERIFY(!VectorRasterizer::prepareGpuFeatherTile(
        layer, QSize(8192, 8192), QRect(0, 0, 8192, 8192), previewSize,
        QTransform(), colourSpace, false, false, &oversizedPrepared,
        &oversizedError));
    QVERIFY(!oversizedError.isEmpty());
    QVERIFY(oversizedError.contains(QStringLiteral("working set"),
                                    Qt::CaseInsensitive));
    QVERIFY(!oversizedPrepared.isValid());

    // Reconstruct the native shader contract with the established integer
    // three-box reference. The GPU stage receives only combined coverage plus
    // a nearest-authored-colour carrier, never a colour image to blur.
    const QRect workingRect = prepared.sourceRect.united(prepared.outputRect);
    QImage coverageCanvas(workingRect.size(), QImage::Format_RGBA8888);
    QVERIFY(!coverageCanvas.isNull());
    coverageCanvas.fill(Qt::transparent);
    coverageCanvas.setColorSpace(colourSpace);
    {
        QPainter painter(&coverageCanvas);
        QVERIFY(painter.isActive());
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(prepared.sourceRect.topLeft() - workingRect.topLeft(),
                          prepared.coverage);
    }
    const QImage blurredCoverage = SpatialFilterFoundation::gaussianBlurReference(
        coverageCanvas, QSize(7, 7), SpatialEdgeMode::Transparent,
        SpatialAlphaMode::StraightRgba);
    QVERIFY(!blurredCoverage.isNull());

    const QImage cpu = VectorRasterizer::renderLayerRegion(
        layer, previewSize, tileRect, previewSize, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!cpu.isNull());
    QCOMPARE(cpu.size(), tileRect.size());
    for (int y = 0; y < tileRect.height(); ++y) {
        const uchar *carrier = prepared.colourCarrier.constScanLine(y);
        const uchar *actual = cpu.constScanLine(y);
        const int coverageY = tileRect.y() + y - workingRect.y();
        const uchar *coverage = blurredCoverage.constScanLine(coverageY);
        for (int x = 0; x < tileRect.width(); ++x) {
            const int coverageX = tileRect.x() + x - workingRect.x();
            const int expectedAlpha = std::lround(
                (coverage[coverageX * 4 + 3] / 255.0)
                * (carrier[x * 4 + 3] / 255.0) * 255.0);
            QCOMPARE(actual[x * 4], carrier[x * 4]);
            QCOMPARE(actual[x * 4 + 1], carrier[x * 4 + 1]);
            QCOMPARE(actual[x * 4 + 2], carrier[x * 4 + 2]);
            QVERIFY(std::abs(static_cast<int>(actual[x * 4 + 3])
                             - expectedAlpha) <= 1);
        }
    }

    // Feather and geometry revisions must invalidate only tiles touched by the
    // feather-expanded silhouette. A distant cached tile remains reusable.
    QImage source(QSize(768, 256), QImage::Format_RGBA8888);
    QVERIFY(!source.isNull());
    source.fill(Qt::transparent);
    source.setColorSpace(colourSpace);
    LayerNode localLayer;
    localLayer.id = QUuid::createUuid();
    localLayer.type = LayerType::Vector;
    localLayer.name = QStringLiteral("Local Feather Cache");
    VectorShape localShape;
    localShape.type = VectorShapeType::Ellipse;
    localShape.bounds = QRectF(24.0, 72.0, 64.0, 64.0);
    localShape.fill.enabled = true;
    localShape.fill.colour = QColor(80, 190, 120, 255);
    localShape.stroke.enabled = false;
    localShape.normalise();
    localLayer.vectorData.objects = {localShape};
    localLayer.vectorData.featherRadius = 8.0;
    localLayer.vectorData.normalise();

    TiledCanvasEngine engine(nullptr);
    const QUuid sessionId = QUuid::createUuid();
    const QRect nearTile(0, 0, 256, 256);
    const QRect farTile(512, 0, 256, 256);
    const QImage firstNear = engine.renderRegion(
        source, {localLayer}, nearTile, source.size(), false, 0, nullptr,
        nullptr, sessionId);
    const QImage firstFar = engine.renderRegion(
        source, {localLayer}, farTile, source.size(), false, 0, nullptr,
        nullptr, sessionId);
    QVERIFY(!firstNear.isNull());
    QVERIFY(!firstFar.isNull());
    const TileCache::Stats initialStats = engine.cacheStatsForSession(sessionId);

    localLayer.vectorData.featherRadius = 12.0;
    localLayer.vectorData.normalise();
    ++localLayer.revision;
    const QImage secondFar = engine.renderRegion(
        source, {localLayer}, farTile, source.size(), false, 0, nullptr,
        nullptr, sessionId);
    QVERIFY(!secondFar.isNull());
    QVERIFY(exactImagesEqual(firstFar, secondFar));
    const TileCache::Stats farStats = engine.cacheStatsForSession(sessionId);
    QCOMPARE(farStats.misses, initialStats.misses);
    QCOMPARE(farStats.hits, initialStats.hits + 1);

    const QImage secondNear = engine.renderRegion(
        source, {localLayer}, nearTile, source.size(), false, 0, nullptr,
        nullptr, sessionId);
    QVERIFY(!secondNear.isNull());
    const TileCache::Stats nearStats = engine.cacheStatsForSession(sessionId);
    QCOMPARE(nearStats.misses, farStats.misses + 1);
    QVERIFY(!exactImagesEqual(firstNear, secondNear));

    engine.clear();
    VectorRasterizer::clearCache();
}


void CoreTests::vectorFeatherWorkflowIntegrationAndSvgRoundTrip()
{
    const QSize size(96, 72);
    const QColorSpace colourSpace(QColorSpace::SRgb);
    QImage transparent(size, QImage::Format_RGBA8888);
    QVERIFY(!transparent.isNull());
    transparent.fill(Qt::transparent);
    transparent.setColorSpace(colourSpace);

    LayerNode vector;
    vector.id = QUuid::createUuid();
    vector.type = LayerType::Vector;
    vector.name = QStringLiteral("Feather Workflow");
    vector.opacity = 0.6;
    vector.transform = QTransform::fromTranslate(4.0, 3.0);
    VectorShape shape;
    shape.type = VectorShapeType::RoundedRectangle;
    shape.bounds = QRectF(24.0, 20.0, 38.0, 26.0);
    shape.cornerRadii.setAll(7.0);
    shape.fill.enabled = true;
    shape.fill.colour = QColor(215, 64, 38, 220);
    shape.fill.opacity = 0.82;
    shape.stroke.enabled = true;
    shape.stroke.colour = QColor(24, 86, 224, 205);
    shape.stroke.opacity = 0.75;
    shape.stroke.width = 6.0;
    shape.stroke.alignment = VectorStrokeAlignment::Centre;
    shape.normalise();
    vector.vectorData.objects = {shape};
    vector.vectorData.featherRadius = 6.0;
    vector.vectorData.normalise();
    QVERIFY(vector.vectorData.isSafe());

    // Prove the unmasked Feather halo exists before the ordinary raster mask
    // is introduced. The mask will then clip that already-feathered coverage.
    const QImage unmasked = ImageProcessor::renderPreservingHiddenRgb(
        transparent, {vector}, nullptr, size).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!unmasked.isNull());
    QVERIFY(unmasked.pixelColor(23, 34).alpha() > 0);

    vector.maskImage = QImage(size, QImage::Format_Grayscale8);
    QVERIFY(!vector.maskImage.isNull());
    vector.maskImage.fill(255);
    for (int y = 0; y < vector.maskImage.height(); ++y) {
        std::memset(vector.maskImage.scanLine(y), 0, 26);
    }
    vector.maskReferenceSize = size;
    vector.maskEnabled = true;

    LayerNode group;
    group.id = QUuid::createUuid();
    group.type = LayerType::Group;
    group.groupCompositeMode = GroupCompositeMode::Isolated;
    group.children = {vector};

    const QImage isolated = ImageProcessor::renderPreservingHiddenRgb(
        transparent, {group}, nullptr, size).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!isolated.isNull());
    // The left halo is removed by the separate raster mask, while the right
    // halo remains outside authored geometry and inside Feather support.
    QCOMPARE(isolated.pixelColor(23, 34).alpha(), 0);
    const QColor halo = isolated.pixelColor(72, 34);
    QVERIFY(halo.alpha() > 0);
    QVERIFY(halo.alpha() < 160);

    group.groupCompositeMode = GroupCompositeMode::PassThrough;
    const QImage passThrough = ImageProcessor::renderPreservingHiddenRgb(
        transparent, {group}, nullptr, size).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!passThrough.isNull());
    QVERIFY2(exactImagesEqual(isolated, passThrough),
             "Pass Through changed a single-child feathered vector composite.");

    // Blend mode and opacity remain ordinary post-Feather compositor state.
    QImage colouredBase(size, QImage::Format_RGBA8888);
    colouredBase.fill(QColor(168, 142, 116, 255));
    colouredBase.setColorSpace(colourSpace);
    LayerNode multiplyVector = vector;
    multiplyVector.maskImage = {};
    multiplyVector.maskReferenceSize = {};
    multiplyVector.maskEnabled = false;
    multiplyVector.blendMode = BlendMode::Multiply;
    LayerNode normalVector = multiplyVector;
    normalVector.blendMode = BlendMode::Copy;
    LayerNode blendBase;
    blendBase.type = LayerType::BaseImage;
    const QImage multiplyResult = ImageProcessor::renderPreservingHiddenRgb(
        colouredBase, {multiplyVector, blendBase}, nullptr, size)
                                      .convertToFormat(QImage::Format_RGBA8888);
    const QImage normalResult = ImageProcessor::renderPreservingHiddenRgb(
        colouredBase, {normalVector, blendBase}, nullptr, size)
                                      .convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!multiplyResult.isNull());
    QVERIFY(!normalResult.isNull());
    QVERIFY2(!exactImagesEqual(multiplyResult, normalResult),
             "Vector Feather bypassed the selected layer blend mode.");
    QVERIFY(multiplyResult.pixelColor(45, 34) != colouredBase.pixelColor(45, 34));

    // Canvas fitting/reveal uses rendered vector bounds, including document-pixel
    // Feather outside the semantic path instead of clipping to geometry bounds.
    PhotoDocument fitDocument;
    QImage fitSource(40, 30, QImage::Format_RGBA8888);
    fitSource.fill(Qt::transparent);
    fitDocument.setSourceImage(fitSource, QStringLiteral("feather-fit.tga"));
    const QUuid fitId = fitDocument.addVectorShape(
        VectorShapeType::Rectangle, QRectF(-3.0, 5.0, 10.0, 8.0),
        QColor(80, 170, 220, 255));
    QVERIFY(!fitId.isNull());
    QVERIFY(fitDocument.updateLayer(fitId, [](LayerNode &layer) {
        layer.transform = QTransform::fromTranslate(2.0, -1.0);
        for (VectorShape &object : layer.vectorData.objects) {
            object.stroke.enabled = false;
            object.normalise();
        }
        layer.vectorData.featherRadius = 4.0;
        layer.vectorData.normalise();
    }));
    CanvasFitRequest fitRequest;
    fitRequest.mode = CanvasFitMode::SelectedLayers;
    fitRequest.layerIds = {fitId};
    CanvasFitResult fitResult;
    QString error;
    QVERIFY2(buildCanvasFitResult(fitDocument, fitRequest, &fitResult,
                                  nullptr, &error), qPrintable(error));
    QCOMPARE(fitResult.documentRect, QRect(-5, 0, 18, 16));

    // Flattened image export and production export both call the same
    // full-resolution hidden-RGB renderer exercised above. Verify the exact
    // full-resolution result still contains the off-geometry Feather halo.
    const QImage exportReference = ImageProcessor::renderPreservingHiddenRgb(
        transparent, {vector}, nullptr, size).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!exportReference.isNull());
    QVERIFY(exportReference.pixelColor(72, 34).alpha() > 0);

    // Editable SVG cannot express Photo Lab's exact combined-silhouette
    // three-box Feather with standard filter primitives. Preserve the semantic
    // value in round-trip metadata, expose it explicitly, and warn rather than
    // silently approximating or rasterising it for external viewers.
    LayerNode svgLayer = vector;
    svgLayer.maskImage = {};
    svgLayer.maskReferenceSize = {};
    svgLayer.maskEnabled = false;
    svgLayer.opacity = 1.0;
    svgLayer.transform.reset();
    SvgExportResult svgExport;
    error.clear();
    const QByteArray svg = SvgWorkflow::exportData(
        size, {svgLayer}, {}, &svgExport, &error);
    QVERIFY2(!svg.isEmpty(), qPrintable(error));
    QVERIFY(svg.contains("data-vfx-feather-radius=\"6\""));
    QVERIFY(!svg.contains("<filter"));
    const bool warnedAboutFeather = std::any_of(
        svgExport.warnings.cbegin(), svgExport.warnings.cend(),
        [](const QString &warning) {
            return warning.contains(QStringLiteral("Vector Feather"),
                                    Qt::CaseInsensitive)
                && warning.contains(QStringLiteral("standard SVG"),
                                    Qt::CaseInsensitive);
        });
    QVERIFY(warnedAboutFeather);

    SvgImportResult svgImport;
    error.clear();
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("feather.svg"),
                                     &svgImport, &error), qPrintable(error));
    QCOMPARE(svgImport.layers.size(), 1);
    QCOMPARE(svgImport.layers.constFirst().type, LayerType::Vector);
    QCOMPARE(svgImport.layers.constFirst().vectorData.featherRadius, 6.0);
    QCOMPARE(svgImport.layers.constFirst().vectorData.objects.size(), 1);
    const VectorShape importedShape =
        svgImport.layers.constFirst().vectorData.objects.constFirst();
    QCOMPARE(importedShape.type, shape.type);
    QCOMPARE(importedShape.bounds, shape.bounds);
    QCOMPARE(importedShape.fill, shape.fill);
    QCOMPARE(importedShape.stroke, shape.stroke);
}


void CoreTests::vectorFeatherHardeningAndRegressionCoverage()
{
    VectorRasterizer::clearCache();
    const QSize size(144, 112);
    const QRect fullRegion(QPoint(), size);
    const QColorSpace colourSpace(QColorSpace::SRgb);

    auto render = [&](const LayerNode &layer,
                      const QImage::Format format,
                      const QRect &region = QRect()) {
        const QRect requested = region.isEmpty() ? fullRegion : region;
        return VectorRasterizer::renderLayerRegion(
            layer, size, requested, size, QTransform(), format, colourSpace);
    };

    // Returning from a non-zero Feather edit to exactly 0 px must recover the
    // accepted semantic raster path byte-for-byte in both supported bit depths.
    LayerNode identityLayer;
    identityLayer.id = QUuid::createUuid();
    identityLayer.type = LayerType::Vector;
    VectorShape identityShape;
    identityShape.type = VectorShapeType::RoundedRectangle;
    identityShape.bounds = QRectF(31.0, 24.0, 67.0, 53.0);
    identityShape.cornerRadii.setAll(9.0);
    identityShape.fill.enabled = true;
    identityShape.fill.colour = QColor(210, 42, 73, 187);
    identityShape.fill.opacity = 0.73;
    identityShape.stroke.enabled = true;
    identityShape.stroke.colour = QColor(31, 111, 228, 214);
    identityShape.stroke.opacity = 0.81;
    identityShape.stroke.width = 7.0;
    identityShape.stroke.alignment = VectorStrokeAlignment::Centre;
    identityShape.normalise();
    identityLayer.vectorData.objects = {identityShape};
    identityLayer.vectorData.normalise();
    const QImage zero8 = render(identityLayer, QImage::Format_RGBA8888);
    const QImage zero16 = render(identityLayer, QImage::Format_RGBA64);
    QVERIFY(!zero8.isNull());
    QVERIFY(!zero16.isNull());

    identityLayer.vectorData.featherRadius = 11.75;
    identityLayer.vectorData.normalise();
    ++identityLayer.revision;
    QVERIFY(!render(identityLayer, QImage::Format_RGBA8888).isNull());
    QVERIFY(!render(identityLayer, QImage::Format_RGBA64).isNull());
    identityLayer.vectorData.featherRadius = 0.0;
    identityLayer.vectorData.normalise();
    ++identityLayer.revision;
    QVERIFY2(exactImagesEqual(zero8, render(identityLayer, QImage::Format_RGBA8888)),
             "Returning Vector Feather to exactly 0 px changed the RGBA8 raster path.");
    QVERIFY2(exactImagesEqual(zero16, render(identityLayer, QImage::Format_RGBA64)),
             "Returning Vector Feather to exactly 0 px changed the RGBA64 raster path.");

    // Tiny authored geometry remains renderable, and the maximum persisted
    // radius is evaluated from compact semantic bounds rather than allocating a
    // million-pixel empty halo image.
    LayerNode tinyLayer;
    tinyLayer.id = QUuid::createUuid();
    tinyLayer.type = LayerType::Vector;
    VectorShape tiny;
    tiny.type = VectorShapeType::Rectangle;
    tiny.bounds = QRectF(70.25, 54.25, 0.75, 0.75);
    tiny.fill.enabled = true;
    tiny.fill.colour = QColor(245, 180, 24, 255);
    tiny.stroke.enabled = false;
    tiny.normalise();
    tinyLayer.vectorData.objects = {tiny};
    tinyLayer.vectorData.featherRadius = 2.0;
    tinyLayer.vectorData.normalise();
    const QImage tinyFeather = render(tinyLayer, QImage::Format_RGBA8888,
                                      QRect(64, 48, 16, 16));
    QVERIFY(!tinyFeather.isNull());
    bool tinyCoverage = false;
    for (int y = 0; y < tinyFeather.height() && !tinyCoverage; ++y) {
        for (int x = 0; x < tinyFeather.width(); ++x) {
            if (tinyFeather.pixelColor(x, y).alpha() > 0) {
                tinyCoverage = true;
                break;
            }
        }
    }
    QVERIFY(tinyCoverage);

    tinyLayer.vectorData.featherRadius = VectorLayerData::MaximumFeatherRadius;
    tinyLayer.vectorData.normalise();
    const QImage maximumRadius = render(tinyLayer, QImage::Format_RGBA64,
                                        QRect(68, 52, 8, 8));
    QVERIFY(!maximumRadius.isNull());
    QCOMPARE(tinyLayer.vectorData.featherRadius,
             VectorLayerData::MaximumFeatherRadius);

    // Fill-only, stroke-only and combined appearance all contribute to one
    // feathered silhouette. Stroke variants remain semantic and editable.
    LayerNode fillOnly = identityLayer;
    fillOnly.id = QUuid::createUuid();
    fillOnly.vectorData.featherRadius = 5.5;
    fillOnly.vectorData.objects.first().stroke.enabled = false;
    fillOnly.vectorData.normalise();
    const QImage fillOnlyImage = render(fillOnly, QImage::Format_RGBA8888);
    QVERIFY(!fillOnlyImage.isNull());
    QVERIFY(fillOnlyImage.pixelColor(28, 50).alpha() > 0);

    LayerNode strokeOnly = identityLayer;
    strokeOnly.id = QUuid::createUuid();
    strokeOnly.vectorData.featherRadius = 5.5;
    strokeOnly.vectorData.objects.first().fill.enabled = false;
    strokeOnly.vectorData.objects.first().stroke.enabled = true;
    strokeOnly.vectorData.objects.first().stroke.pattern = VectorStrokePattern::Dashed;
    strokeOnly.vectorData.objects.first().stroke.dashLength = 9.0;
    strokeOnly.vectorData.objects.first().stroke.gapLength = 5.0;
    strokeOnly.vectorData.objects.first().stroke.cap = VectorStrokeCap::Square;
    strokeOnly.vectorData.objects.first().stroke.join = VectorStrokeJoin::Bevel;
    strokeOnly.vectorData.objects.first().normalise();
    strokeOnly.vectorData.normalise();
    const QImage strokeOnlyImage = render(strokeOnly, QImage::Format_RGBA8888);
    QVERIFY(!strokeOnlyImage.isNull());
    QVERIFY(strokeOnlyImage.pixelColor(27, 50).alpha() > 0);

    LayerNode combined = identityLayer;
    combined.id = QUuid::createUuid();
    combined.vectorData.featherRadius = 5.5;
    combined.vectorData.normalise();
    const QImage combined8 = render(combined, QImage::Format_RGBA8888);
    const QImage combined16 = render(combined, QImage::Format_RGBA64);
    QVERIFY(!combined8.isNull());
    QVERIFY(!combined16.isNull());
    QVERIFY(combined8.pixelColor(27, 50).alpha() > 0);

    // 8/16-bit references share coverage. Quantisation differs, but normalised
    // Alpha and authored RGB must agree to the expected conversion tolerance.
    for (int y = 0; y < size.height(); y += 7) {
        const auto *row16 = reinterpret_cast<const QRgba64 *>(
            combined16.constScanLine(y));
        for (int x = 0; x < size.width(); x += 7) {
            const QColor pixel8 = combined8.pixelColor(x, y);
            const QRgba64 pixel16 = row16[x];
            QVERIFY(std::abs(pixel8.alphaF() - pixel16.alpha() / 65535.0) < 0.012);
            if (pixel8.alpha() > 0 && pixel16.alpha() > 0) {
                QVERIFY(std::abs(pixel8.redF() - pixel16.red() / 65535.0) < 0.012);
                QVERIFY(std::abs(pixel8.greenF() - pixel16.green() / 65535.0) < 0.012);
                QVERIFY(std::abs(pixel8.blueF() - pixel16.blue() / 65535.0) < 0.012);
            }
        }
    }

    // Overlapping closed subpaths remain governed by the stored winding rule
    // before Feather is applied. Even-odd overlap opens a hole; NonZero keeps
    // the same overlap filled.
    auto rectanglePath = [](const QRectF &bounds) {
        VectorBezierPath path;
        path.closed = true;
        for (const QPointF point : {bounds.topLeft(), bounds.topRight(),
                                     bounds.bottomRight(), bounds.bottomLeft()}) {
            VectorPathNode node;
            node.anchor = point;
            node.clearHandles();
            path.nodes.push_back(node);
        }
        path.normalise();
        return path;
    };
    VectorShape compoundShape;
    compoundShape.type = VectorShapeType::Path;
    compoundShape.bezierPath = rectanglePath(QRectF(32, 24, 64, 56));
    compoundShape.additionalBezierPaths = {rectanglePath(QRectF(52, 40, 32, 24))};
    compoundShape.fill.enabled = true;
    compoundShape.fill.colour = QColor(80, 205, 135, 230);
    compoundShape.stroke.enabled = false;
    compoundShape.pathFillRule = VectorPathFillRule::EvenOdd;
    compoundShape.normalise();
    LayerNode compoundLayer;
    compoundLayer.id = QUuid::createUuid();
    compoundLayer.type = LayerType::Vector;
    compoundLayer.vectorData.objects = {compoundShape};
    compoundLayer.vectorData.featherRadius = 4.0;
    compoundLayer.vectorData.normalise();
    const QImage evenOdd = render(compoundLayer, QImage::Format_RGBA8888);
    QVERIFY(!evenOdd.isNull());
    QCOMPARE(evenOdd.pixelColor(68, 52).alpha(), 0);
    QVERIFY(evenOdd.pixelColor(54, 52).alpha() > 0);

    compoundLayer.vectorData.objects.first().pathFillRule = VectorPathFillRule::NonZero;
    compoundLayer.vectorData.objects.first().normalise();
    compoundLayer.vectorData.normalise();
    ++compoundLayer.revision;
    const QImage nonZero = render(compoundLayer, QImage::Format_RGBA8888);
    QVERIFY(!nonZero.isNull());
    QVERIFY(nonZero.pixelColor(68, 52).alpha() > 0);

    // Repeated Feather revisions must remain bounded by the shared vector
    // raster cache budget instead of retaining every scrub frame indefinitely.
    VectorRasterizer::clearCache();
    LayerNode scrubbed = combined;
    for (int revision = 1; revision <= 96; ++revision) {
        scrubbed.vectorData.featherRadius = 1.0 + (revision % 24) * 0.5;
        scrubbed.vectorData.normalise();
        scrubbed.revision = static_cast<quint64>(revision);
        const QImage frame = render(scrubbed, QImage::Format_RGBA8888);
        QVERIFY(!frame.isNull());
    }
    QVERIFY(VectorRasterizer::cacheBytes() <= qsizetype(128) * 1024 * 1024);
    QVERIFY(VectorRasterizer::cacheEntryCount() <= 2048);
    VectorRasterizer::clearCache();
}


void CoreTests::bezierPathRoundTripsVersionNineAndRejectsPreVersionNine()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(180, 120);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    VectorBezierPath path;
    VectorPathNode first;
    first.anchor = QPointF(18.0, 82.0);
    first.outHandle = QPointF(42.0, 28.0);
    first.outHandleActive = true;
    first.mode = VectorNodeMode::Smooth;
    VectorPathNode second;
    second.anchor = QPointF(92.0, 24.0);
    second.inHandle = QPointF(58.0, 18.0);
    second.outHandle = QPointF(126.0, 30.0);
    second.inHandleActive = true;
    second.outHandleActive = true;
    second.mode = VectorNodeMode::Smooth;
    VectorPathNode third;
    third.anchor = QPointF(154.0, 88.0);
    third.inHandle = QPointF(126.0, 76.0);
    third.inHandleActive = true;
    third.mode = VectorNodeMode::Corner;
    path.nodes = {first, second, third};
    path.normalise();
    QVERIFY(path.isSafe());

    VectorShape fillPreference;
    fillPreference.type = VectorShapeType::Path;
    fillPreference.bezierPath = path;
    fillPreference.bezierPath.closed = true;
    fillPreference.fill.enabled = true;
    fillPreference.stroke.enabled = true;
    fillPreference.stroke.alignment = VectorStrokeAlignment::Inside;
    fillPreference.normalise();
    QVERIFY(fillPreference.fill.enabled);
    QCOMPARE(fillPreference.stroke.alignment, VectorStrokeAlignment::Inside);
    fillPreference.bezierPath.closed = false;
    fillPreference.normalise();
    QVERIFY(fillPreference.fill.enabled);
    QVERIFY(fillPreference.isOpenPath());
    QCOMPARE(fillPreference.stroke.alignment, VectorStrokeAlignment::Inside);
    QVERIFY(!fillPreference.styledPathForWorldTransform(QTransform())
                 .contains(QPointF(90.0, 70.0)));
    fillPreference.bezierPath.closed = true;
    fillPreference.normalise();
    QVERIFY(fillPreference.fill.enabled);
    QCOMPARE(fillPreference.stroke.alignment, VectorStrokeAlignment::Inside);

    const QUuid pathId = document.addVectorPath(path, QColor(190, 40, 150, 220));
    QVERIFY(!pathId.isNull());
    QVERIFY(document.updateLayer(pathId, [](LayerNode &layer) {
        VectorShape &shape = layer.vectorData.objects.first();
        shape.stroke.width = 7.5;
        shape.stroke.cap = VectorStrokeCap::Round;
        shape.stroke.join = VectorStrokeJoin::Bevel;
        ++shape.revision;
        layer.vectorData.normalise();
    }));
    const LayerNode expected = document.layerById(pathId);
    QCOMPARE(expected.vectorData.schema, VectorLayerData::CurrentSchema);
    QCOMPARE(expected.vectorData.objects.constFirst().type, VectorShapeType::Path);

    bool vectorJsonOk = false;
    QJsonObject vectorJson = expected.vectorData.toJson(&vectorJsonOk);
    QVERIFY(vectorJsonOk);
    vectorJson.insert(QStringLiteral("schema"), 1);
    bool legacyVectorOk = false;
    VectorLayerData::fromJson(vectorJson, &legacyVectorOk);
    QVERIFY(!legacyVectorOk);

    VectorLayerData legacyShapeData;
    VectorShape legacyRectangle;
    legacyRectangle.type = VectorShapeType::Rectangle;
    legacyRectangle.bounds = QRectF(1.0, 2.0, 30.0, 20.0);
    legacyRectangle.normalise();
    legacyShapeData.objects = {legacyRectangle};
    legacyShapeData.normalise();
    QJsonObject legacyShapeJson = legacyShapeData.toJson(&vectorJsonOk);
    QVERIFY(vectorJsonOk);
    legacyShapeJson.insert(QStringLiteral("schema"), 1);
    legacyShapeJson.remove(QStringLiteral("featherRadius"));
    const VectorLayerData restoredLegacyShape =
        VectorLayerData::fromJson(legacyShapeJson, &legacyVectorOk);
    QVERIFY(legacyVectorOk);
    QCOMPARE(restoredLegacyShape.objects.size(), 1);
    QCOMPARE(restoredLegacyShape.objects.constFirst().type,
             VectorShapeType::Rectangle);

    const QString projectPath = directory.filePath(QStringLiteral("bezier-v9.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    QFile file(projectPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument encoded = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(encoded.isObject());
    QCOMPARE(encoded.object().value(QStringLiteral("version")).toInt(),
             PhotoDocument::ProjectFormatVersion);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QCOMPARE(restored.layerById(pathId).vectorData, expected.vectorData);

    QJsonObject dishonest = encoded.object();
    dishonest.insert(QStringLiteral("version"), 8);
    const QString dishonestPath = directory.filePath(QStringLiteral("illegal-bezier-v8.vfxphoto"));
    QFile dishonestFile(dishonestPath);
    QVERIFY(dishonestFile.open(QIODevice::WriteOnly));
    const QByteArray bytes = QJsonDocument(dishonest).toJson(QJsonDocument::Compact);
    QCOMPARE(dishonestFile.write(bytes), static_cast<qint64>(bytes.size()));
    dishonestFile.close();
    PhotoDocument rejected;
    QVERIFY(!rejected.loadProject(dishonestPath, &error));
    QVERIFY(error.contains(QStringLiteral("version-9"), Qt::CaseInsensitive));
}

void CoreTests::liveVectorCornersRoundTripBakeAndRejectPreVersionTen()
{
    VectorBezierPath path;
    path.closed = true;
    for (const QPointF anchor : {QPointF(10.0, 10.0), QPointF(110.0, 10.0),
                                 QPointF(110.0, 90.0), QPointF(10.0, 90.0)}) {
        VectorPathNode node;
        node.anchor = anchor;
        node.inHandle = anchor;
        node.outHandle = anchor;
        path.nodes.push_back(node);
    }
    path.normalise();
    QVERIFY(path.isSafe());
    for (int index = 0; index < path.nodes.size(); ++index) {
        QVERIFY(path.cornerableNode(index));
        QCOMPARE(path.maximumCornerRadius(index), 40.0);
    }

    path.nodes[0].cornerRadius = 18.0;
    path.nodes[0].cornerStyle = VectorCornerStyle::Rounded;
    path.nodes[1].cornerRadius = 16.0;
    path.nodes[1].cornerStyle = VectorCornerStyle::Chamfer;
    path.nodes[2].cornerRadius = 14.0;
    path.nodes[2].cornerStyle = VectorCornerStyle::Concave;
    path.nodes[3].cornerRadius = 12.0;
    path.nodes[3].cornerStyle = VectorCornerStyle::Cutout;
    path.normalise();
    QVERIFY(path.hasLiveCorners());
    QVERIFY(path.cornerHandlePoint(0) != path.nodes.at(0).anchor);

    const QPainterPath base = path.basePainterPath();
    const QPainterPath live = path.painterPath();
    QVERIFY(!base.isEmpty());
    QVERIFY(!live.isEmpty());
    QVERIFY(live.elementCount() > base.elementCount());
    QCOMPARE(live.boundingRect(), base.boundingRect());

    VectorLayerData data;
    VectorShape shape;
    shape.type = VectorShapeType::Path;
    shape.bezierPath = path;
    shape.fill.enabled = true;
    shape.fill.colour = QColor(35, 170, 210, 230);
    shape.stroke.enabled = true;
    shape.stroke.width = 3.0;
    shape.normalise();
    data.objects = {shape};
    data.normalise();
    QCOMPARE(data.schema, VectorLayerData::CurrentSchema);
    QCOMPARE(VectorLayerData::CurrentSchema, 8u);

    bool jsonOk = false;
    const QJsonObject encodedVector = data.toJson(&jsonOk);
    QVERIFY(jsonOk);
    bool decodedOk = false;
    const VectorLayerData decoded = VectorLayerData::fromJson(encodedVector, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(decoded, data);
    QVERIFY(decoded.objects.constFirst().bezierPath.hasLiveCorners());

    QJsonObject dishonestSchema = encodedVector;
    dishonestSchema.insert(QStringLiteral("schema"), 2);
    VectorLayerData::fromJson(dishonestSchema, &decodedOk);
    QVERIFY(!decodedOk);

    VectorLayerData styleOnly = data;
    for (VectorPathNode &node : styleOnly.objects.first().bezierPath.nodes) {
        node.cornerRadius = 0.0;
        node.cornerStyle = VectorCornerStyle::Rounded;
    }
    styleOnly.objects.first().bezierPath.nodes.first().cornerStyle =
        VectorCornerStyle::Chamfer;
    styleOnly.normalise();
    QJsonObject dishonestStyleOnly = styleOnly.toJson(&jsonOk);
    QVERIFY(jsonOk);
    dishonestStyleOnly.insert(QStringLiteral("schema"), 2);
    VectorLayerData::fromJson(dishonestStyleOnly, &decodedOk);
    QVERIFY(!decodedOk);

    VectorBezierPath baked = path;
    const QPainterPath expectedBakedGeometry = baked.painterPath();
    QVERIFY(baked.bakeCorners());
    QVERIFY(!baked.hasLiveCorners());
    QVERIFY(baked.nodes.size() > path.nodes.size());
    const QPainterPath bakedGeometry = baked.painterPath();
    QCOMPARE(bakedGeometry.elementCount(), expectedBakedGeometry.elementCount());
    for (int sample = 0; sample <= 40; ++sample) {
        const double percent = sample / 40.0;
        QVERIFY(QLineF(expectedBakedGeometry.pointAtPercent(percent),
                       bakedGeometry.pointAtPercent(percent)).length() < 0.01);
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(128, 104);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QUuid layerId = document.addVectorPath(path, shape.fill.colour);
    QVERIFY(!layerId.isNull());
    const QString projectPath = directory.filePath(QStringLiteral("live-corners-v11.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));

    QFile projectFile(projectPath);
    QVERIFY(projectFile.open(QIODevice::ReadOnly));
    const QJsonDocument projectJson = QJsonDocument::fromJson(projectFile.readAll());
    projectFile.close();
    QVERIFY(projectJson.isObject());
    QCOMPARE(projectJson.object().value(QStringLiteral("version")).toInt(), 15);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QVERIFY(restored.layerById(layerId).vectorData.objects.constFirst()
                .bezierPath.hasLiveCorners());

    QJsonObject dishonestProject = projectJson.object();
    dishonestProject.insert(QStringLiteral("version"), 9);
    const QString dishonestPath = directory.filePath(
        QStringLiteral("illegal-live-corners-v9.vfxphoto"));
    QFile dishonestFile(dishonestPath);
    QVERIFY(dishonestFile.open(QIODevice::WriteOnly));
    const QByteArray bytes = QJsonDocument(dishonestProject).toJson(QJsonDocument::Compact);
    QCOMPARE(dishonestFile.write(bytes), static_cast<qint64>(bytes.size()));
    dishonestFile.close();
    PhotoDocument rejected;
    QVERIFY(!rejected.loadProject(dishonestPath, &error));
    QVERIFY(error.contains(QStringLiteral("version-10"), Qt::CaseInsensitive));
}

void CoreTests::bezierPathInsertionAndNodeModesPreserveCurves()
{
    VectorBezierPath path;
    VectorPathNode first;
    first.anchor = QPointF(0.0, 0.0);
    first.outHandle = QPointF(30.0, -20.0);
    first.outHandleActive = true;
    first.mode = VectorNodeMode::Smooth;
    VectorPathNode second;
    second.anchor = QPointF(100.0, 40.0);
    second.inHandle = QPointF(70.0, 70.0);
    second.inHandleActive = true;
    second.mode = VectorNodeMode::Smooth;
    path.nodes = {first, second};
    path.normalise();
    const QPainterPath before = path.painterPath();
    QVERIFY(!before.isEmpty());

    int inserted = -1;
    QVERIFY(path.insertNodeOnSegment(0, 0.5, &inserted));
    QCOMPARE(inserted, 1);
    QCOMPARE(path.nodes.size(), 3);
    QVERIFY(path.isSafe());
    const QPointF midpoint = path.nodes.at(inserted).anchor;
    QVERIFY(before.boundingRect().adjusted(-0.01, -0.01, 0.01, 0.01).contains(midpoint));
    QCOMPARE(path.nodes.at(inserted).mode, VectorNodeMode::Smooth);
    const QPainterPath afterInsertion = path.painterPath();
    for (int sample = 0; sample <= 20; ++sample) {
        const double percent = sample / 20.0;
        QVERIFY(QLineF(before.pointAtPercent(percent),
                       afterInsertion.pointAtPercent(percent)).length() < 0.02);
    }

    VectorPathNode symmetric;
    symmetric.anchor = QPointF(10.0, 10.0);
    symmetric.inHandle = QPointF(5.0, 10.0);
    symmetric.outHandle = QPointF(15.0, 10.0);
    symmetric.inHandleActive = symmetric.outHandleActive = true;
    symmetric.mode = VectorNodeMode::Symmetric;
    symmetric.setOutHandle(QPointF(10.0, 20.0));
    QCOMPARE(symmetric.inHandle, QPointF(10.0, 0.0));
    symmetric.mode = VectorNodeMode::Corner;
    const QPointF oldOut = symmetric.outHandle;
    symmetric.setInHandle(QPointF(2.0, 3.0));
    QCOMPARE(symmetric.outHandle, oldOut);

    VectorPathNode sharp = symmetric;
    sharp.cornerRadius = 7.5;
    sharp.cornerStyle = VectorCornerStyle::Cutout;
    sharp.makeSharp();
    QCOMPARE(sharp.mode, VectorNodeMode::Corner);
    QVERIFY(!sharp.inHandleActive);
    QVERIFY(!sharp.outHandleActive);
    QCOMPARE(sharp.inHandle, sharp.anchor);
    QCOMPARE(sharp.outHandle, sharp.anchor);
    QCOMPARE(sharp.cornerRadius, 0.0);
    QCOMPARE(sharp.cornerStyle, VectorCornerStyle::Rounded);

    bool jsonOk = false;
    const QJsonObject encoded = path.toJson(&jsonOk);
    QVERIFY(jsonOk);
    bool decodedOk = false;
    const VectorBezierPath decoded = VectorBezierPath::fromJson(encoded, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(decoded, path);

    QJsonObject oversized;
    QJsonArray nodes;
    for (int index = 0; index <= VectorBezierPath::MaximumNodeCount; ++index) {
        nodes.append(QJsonObject());
    }
    oversized.insert(QStringLiteral("closed"), false);
    oversized.insert(QStringLiteral("nodes"), nodes);
    VectorBezierPath::fromJson(oversized, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject duplicateNodes = encoded;
    QJsonArray duplicateArray = duplicateNodes.value(QStringLiteral("nodes")).toArray();
    duplicateArray.append(duplicateArray.at(0));
    duplicateNodes.insert(QStringLiteral("nodes"), duplicateArray);
    VectorBezierPath::fromJson(duplicateNodes, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject impossibleClosed;
    impossibleClosed.insert(QStringLiteral("closed"), true);
    impossibleClosed.insert(QStringLiteral("nodes"), QJsonArray{duplicateArray.at(0)});
    VectorBezierPath::fromJson(impossibleClosed, &decodedOk);
    QVERIFY(!decodedOk);
}

void CoreTests::bezierPathSelectedNodeMovementIsAtomic()
{
    VectorBezierPath path;
    VectorPathNode first;
    first.anchor = QPointF(12.0, 18.0);
    first.inHandle = QPointF(4.0, 20.0);
    first.outHandle = QPointF(28.0, 6.0);
    first.inHandleActive = true;
    first.outHandleActive = true;
    first.mode = VectorNodeMode::Corner;
    first.cornerRadius = 4.5;

    VectorPathNode middle;
    middle.anchor = QPointF(60.0, 42.0);
    middle.inHandle = QPointF(46.0, 30.0);
    middle.outHandle = QPointF(76.0, 54.0);
    middle.inHandleActive = true;
    middle.outHandleActive = true;
    middle.mode = VectorNodeMode::Smooth;

    VectorPathNode last;
    last.anchor = QPointF(110.0, 22.0);
    last.inHandle = QPointF(94.0, 34.0);
    last.inHandleActive = true;
    last.mode = VectorNodeMode::Corner;
    path.nodes = {first, middle, last};
    path.closed = true;
    path.normalise();
    QVERIFY(path.isSafe());

    const VectorBezierPath before = path;
    const QPointF delta(7.0, -3.0);
    QVERIFY(path.moveNodesBy(QSet<int>{0, 2}, delta));
    QCOMPARE(path.nodes.at(0).anchor, before.nodes.at(0).anchor + delta);
    QCOMPARE(path.nodes.at(0).inHandle, before.nodes.at(0).inHandle + delta);
    QCOMPARE(path.nodes.at(0).outHandle, before.nodes.at(0).outHandle + delta);
    QCOMPARE(path.nodes.at(2).anchor, before.nodes.at(2).anchor + delta);
    QCOMPARE(path.nodes.at(2).inHandle, before.nodes.at(2).inHandle + delta);
    QCOMPARE(path.nodes.at(1), before.nodes.at(1));
    QCOMPARE(path.nodes.at(0).cornerRadius, before.nodes.at(0).cornerRadius);
    QCOMPARE(path.nodes.at(0).id, before.nodes.at(0).id);
    QCOMPARE(path.nodes.at(2).id, before.nodes.at(2).id);
    QVERIFY(path.closed);
    QVERIFY(path.isSafe());

    VectorBezierPath transformed = before;
    const QTransform localToDocument(
        1.15, 0.18, 0.0007,
        -0.22, 0.92, -0.0004,
        34.0, -17.0, 1.0);
    bool inverseOk = false;
    const QTransform documentToLocal = localToDocument.inverted(&inverseOk);
    QVERIFY(inverseOk);
    const QPointF documentDelta(-10.0, 1.0);
    const QTransform localNudge = localToDocument
        * QTransform::fromTranslate(documentDelta.x(), documentDelta.y())
        * documentToLocal;
    const QPointF firstDocumentBefore = localToDocument.map(
        transformed.nodes.at(0).anchor);
    const QPointF firstHandleDocumentBefore = localToDocument.map(
        transformed.nodes.at(0).outHandle);
    const QPointF lastDocumentBefore = localToDocument.map(
        transformed.nodes.at(2).anchor);
    QVERIFY(transformed.transformNodes(QSet<int>{0, 2}, localNudge));
    QVERIFY(QLineF(localToDocument.map(transformed.nodes.at(0).anchor),
                   firstDocumentBefore + documentDelta).length() < 1.0e-7);
    QVERIFY(QLineF(localToDocument.map(transformed.nodes.at(0).outHandle),
                   firstHandleDocumentBefore + documentDelta).length() < 1.0e-7);
    QVERIFY(QLineF(localToDocument.map(transformed.nodes.at(2).anchor),
                   lastDocumentBefore + documentDelta).length() < 1.0e-7);
    QCOMPARE(transformed.nodes.at(1), before.nodes.at(1));

    const VectorBezierPath afterMove = path;
    QVERIFY(!path.moveNodesBy(QSet<int>{-1, 99}, QPointF(1.0, 0.0)));
    QCOMPARE(path, afterMove);
    QVERIFY(!path.moveNodesBy(QSet<int>{0},
                              QPointF(std::numeric_limits<double>::infinity(), 0.0)));
    QCOMPARE(path, afterMove);

    VectorBezierPath nearLimit = before;
    nearLimit.nodes.first().anchor = QPointF(999999999.0, 10.0);
    nearLimit.nodes.first().inHandle = nearLimit.nodes.first().anchor;
    nearLimit.nodes.first().outHandle = nearLimit.nodes.first().anchor;
    nearLimit.normalise();
    QVERIFY(nearLimit.isSafe());
    const VectorBezierPath safeBefore = nearLimit;
    QVERIFY(!nearLimit.moveNodesBy(QSet<int>{0, 1}, QPointF(10.0, 0.0)));
    QCOMPARE(nearLimit, safeBefore);
}

void CoreTests::bezierPathDirectionAndJoiningPreserveGeometry()
{
    VectorBezierPath original;
    VectorPathNode first;
    first.anchor = QPointF(8.0, 34.0);
    first.outHandle = QPointF(28.0, 4.0);
    first.outHandleActive = true;
    first.mode = VectorNodeMode::Corner;
    VectorPathNode second;
    second.anchor = QPointF(72.0, 24.0);
    second.inHandle = QPointF(48.0, 54.0);
    second.outHandle = QPointF(92.0, -2.0);
    second.inHandleActive = true;
    second.outHandleActive = true;
    second.mode = VectorNodeMode::Smooth;
    VectorPathNode third;
    third.anchor = QPointF(126.0, 48.0);
    third.inHandle = QPointF(106.0, 68.0);
    third.inHandleActive = true;
    original.nodes = {first, second, third};
    original.normalise();
    QVERIFY(original.isSafe());

    const QPainterPath forwardGeometry = original.painterPath();
    VectorBezierPath reversed = original;
    reversed.reverseDirection();
    QVERIFY(reversed.isSafe());
    QCOMPARE(reversed.nodes.constFirst().id, original.nodes.constLast().id);
    QCOMPARE(reversed.nodes.constFirst().outHandle,
             original.nodes.constLast().inHandle);
    QCOMPARE(reversed.nodes.constFirst().outHandleActive,
             original.nodes.constLast().inHandleActive);
    const QPainterPath reverseGeometry = reversed.painterPath();
    for (int sample = 0; sample <= 40; ++sample) {
        const double percent = sample / 40.0;
        QVERIFY(QLineF(forwardGeometry.pointAtPercent(percent),
                       reverseGeometry.pointAtPercent(1.0 - percent)).length()
                < 0.03);
    }
    reversed.reverseDirection();
    QCOMPARE(reversed, original);

    VectorBezierPath following;
    VectorPathNode fourth;
    fourth.anchor = QPointF(154.0, 72.0);
    fourth.inHandle = QPointF(138.0, 54.0);
    fourth.outHandle = QPointF(170.0, 86.0);
    fourth.inHandleActive = true;
    fourth.outHandleActive = true;
    VectorPathNode fifth;
    fifth.anchor = QPointF(204.0, 64.0);
    fifth.inHandle = QPointF(184.0, 90.0);
    fifth.inHandleActive = true;
    following.nodes = {fourth, fifth};
    following.normalise();
    QVERIFY(following.isSafe());

    VectorBezierPath nonCoincident = original;
    int junction = -1;
    QVERIFY(nonCoincident.joinFollowingPath(following, &junction));
    QCOMPARE(junction, original.nodes.size());
    QCOMPARE(nonCoincident.nodes.size(),
             original.nodes.size() + following.nodes.size());
    QCOMPARE(nonCoincident.nodes.at(junction).id,
             following.nodes.constFirst().id);
    QVERIFY(nonCoincident.isSafe());

    VectorBezierPath touching = following;
    touching.nodes.first().anchor = original.nodes.constLast().anchor;
    touching.nodes.first().inHandle = original.nodes.constLast().anchor
        + QPointF(-12.0, -5.0);
    touching.nodes.first().outHandle = original.nodes.constLast().anchor
        + QPointF(18.0, 9.0);
    touching.normalise();
    QVERIFY(touching.isSafe());
    VectorBezierPath merged = original;
    QVERIFY(merged.joinFollowingPath(touching, &junction));
    QCOMPARE(junction, original.nodes.size() - 1);
    QCOMPARE(merged.nodes.size(),
             original.nodes.size() + touching.nodes.size() - 1);
    const VectorPathNode &mergedJunction = merged.nodes.at(junction);
    QCOMPARE(mergedJunction.id, original.nodes.constLast().id);
    QCOMPARE(mergedJunction.inHandle, original.nodes.constLast().inHandle);
    QCOMPARE(mergedJunction.inHandleActive,
             original.nodes.constLast().inHandleActive);
    QCOMPARE(mergedJunction.outHandle, touching.nodes.constFirst().outHandle);
    QCOMPARE(mergedJunction.outHandleActive,
             touching.nodes.constFirst().outHandleActive);
    QCOMPARE(mergedJunction.mode, VectorNodeMode::Corner);
    QVERIFY(merged.isSafe());

    VectorBezierPath closed = original;
    closed.closed = true;
    closed.normalise();
    QVERIFY(closed.isSafe());
    QVERIFY(!closed.joinFollowingPath(following));
    QVERIFY(!original.joinFollowingPath(closed));
}

void CoreTests::bezierPathRasterizerExposesSemanticCoverageAndSnapPoints()
{
    LayerNode layer;
    layer.type = LayerType::Vector;
    VectorShape shape;
    shape.type = VectorShapeType::Path;
    shape.bezierPath.closed = true;
    for (const QPointF point : {QPointF(16.0, 16.0), QPointF(70.0, 14.0),
                                QPointF(58.0, 70.0), QPointF(20.0, 62.0)}) {
        VectorPathNode node;
        node.anchor = point;
        node.inHandle = point;
        node.outHandle = point;
        shape.bezierPath.nodes.push_back(node);
    }
    shape.fill.enabled = true;
    shape.fill.colour = QColor(30, 180, 90, 210);
    shape.stroke.enabled = true;
    shape.stroke.colour = QColor(170, 30, 130, 255);
    shape.stroke.width = 5.0;
    shape.stroke.alignment = VectorStrokeAlignment::Inside;
    shape.normalise();
    layer.vectorData.objects = {shape};
    layer.vectorData.normalise();
    QVERIFY(layer.vectorData.isSafe());

    const QSize size(96, 96);
    const QImage rendered = VectorRasterizer::renderLayerRegion(
        layer, size, QRect(QPoint(), size), size, QTransform(),
        QImage::Format_RGBA8888, QColorSpace(QColorSpace::SRgb));
    QVERIFY(!rendered.isNull());
    QVERIFY(rendered.pixelColor(35, 35).alpha() > 0);
    QCOMPARE(rendered.pixelColor(2, 2).alpha(), 0);

    const QVector<QPointF> points = shape.snapPoints();
    QCOMPARE(points.size(), 4);
    QCOMPARE(points.constFirst(), QPointF(16.0, 16.0));
    const QRectF content = VectorRasterizer::contentBounds(layer);
    QVERIFY(content.contains(QPointF(35.0, 35.0)));

    const QImage rendered64 = VectorRasterizer::renderLayerRegion(
        layer, size, QRect(QPoint(), size), size, QTransform(),
        QImage::Format_RGBA64, QColorSpace(QColorSpace::SRgb));
    QVERIFY(!rendered64.isNull());
    QCOMPARE(rendered64.format(), QImage::Format_RGBA64);
    QVERIFY(rendered64.pixelColor(35, 35).alpha() > 0);

    const QImage rightTile = VectorRasterizer::renderLayerRegion(
        layer, size, QRect(48, 0, 48, 96), size, QTransform(),
        QImage::Format_RGBA8888, QColorSpace(QColorSpace::SRgb));
    QVERIFY(!rightTile.isNull());
    QCOMPARE(rightTile.pixelColor(10, 35), rendered.pixelColor(58, 35));
}

void CoreTests::bezierPathCopyAndImageSizeRegenerateAndScaleNodes()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(100, 80);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    VectorBezierPath path;
    VectorPathNode first;
    first.anchor = QPointF(10.0, 20.0);
    first.outHandle = QPointF(20.0, 5.0);
    first.outHandleActive = true;
    VectorPathNode second;
    second.anchor = QPointF(70.0, 60.0);
    second.inHandle = QPointF(55.0, 75.0);
    second.inHandleActive = true;
    path.nodes = {first, second};
    path.normalise();
    const QUuid sourceId = document.addVectorPath(path, QColor(Qt::blue));
    QVERIFY(!sourceId.isNull());
    const LayerNode source = document.layerById(sourceId);
    const VectorShape sourceShape = source.vectorData.objects.constFirst();

    VectorBezierPath cornerPath;
    cornerPath.closed = true;
    for (const QPointF anchor : {QPointF(5.0, 5.0), QPointF(45.0, 5.0),
                                 QPointF(45.0, 35.0), QPointF(5.0, 35.0)}) {
        VectorPathNode node;
        node.anchor = anchor;
        node.clearHandles();
        cornerPath.nodes.push_back(node);
    }
    cornerPath.nodes[0].cornerRadius = 8.0;
    cornerPath.nodes[0].cornerStyle = VectorCornerStyle::Chamfer;
    cornerPath.normalise();
    const QUuid cornerId = document.addVectorPath(cornerPath, QColor(Qt::green));
    QVERIFY(!cornerId.isNull());

    QTransform desired;
    desired.translate(12.0, 8.0);
    const QUuid copyId = document.insertVectorLayerCopy(source, desired);
    QVERIFY(!copyId.isNull());
    const VectorShape copied = document.layerById(copyId).vectorData.objects.constFirst();
    QVERIFY(copied.id != sourceShape.id);
    QCOMPARE(copied.bezierPath.nodes.size(), sourceShape.bezierPath.nodes.size());
    for (int index = 0; index < copied.bezierPath.nodes.size(); ++index) {
        QVERIFY(copied.bezierPath.nodes.at(index).id
                != sourceShape.bezierPath.nodes.at(index).id);
    }

    ImageSizeRequest request;
    request.pixelSize = QSize(200, 40);
    request.method = ImageResampleMethod::NearestNeighbour;
    ImageSizeResult result;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    LayerNode scaled;
    std::function<bool(const QVector<LayerNode> &)> find;
    find = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (layer.id == sourceId) { scaled = layer; return true; }
            if (find(layer.children)) return true;
        }
        return false;
    };
    QVERIFY(find(result.layers));
    const VectorPathNode scaledFirst = scaled.vectorData.objects.constFirst()
        .bezierPath.nodes.constFirst();
    QCOMPARE(scaledFirst.anchor, QPointF(20.0, 10.0));
    QCOMPARE(scaledFirst.outHandle, QPointF(40.0, 2.5));

    LayerNode scaledCorner;
    std::function<bool(const QVector<LayerNode> &)> findCorner;
    findCorner = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (layer.id == cornerId) { scaledCorner = layer; return true; }
            if (findCorner(layer.children)) return true;
        }
        return false;
    };
    QVERIFY(findCorner(result.layers));
    const VectorPathNode scaledLiveCorner = scaledCorner.vectorData.objects.constFirst()
        .bezierPath.nodes.constFirst();
    QCOMPARE(scaledLiveCorner.anchor, QPointF(10.0, 2.5));
    QCOMPARE(scaledLiveCorner.cornerRadius, 4.0);
    QCOMPARE(scaledLiveCorner.cornerStyle, VectorCornerStyle::Chamfer);
}

void CoreTests::vectorGeometryCachePreservesLongPathTilesAndInvalidatesEdits()
{
    VectorRasterizer::clearCache();
    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = QStringLiteral("Long cached path");

    VectorShape shape;
    shape.type = VectorShapeType::Path;
    shape.fill.enabled = false;
    shape.stroke.enabled = true;
    shape.stroke.colour = QColor(180, 45, 125, 230);
    shape.stroke.width = 5.0;
    shape.stroke.cap = VectorStrokeCap::Round;
    shape.stroke.join = VectorStrokeJoin::Round;
    for (int index = 0; index < 96; ++index) {
        const double x = 8.0 + index * 5.1;
        const double y = 128.0 + std::sin(index * 0.31) * 72.0;
        VectorPathNode node;
        node.anchor = QPointF(x, y);
        node.inHandle = QPointF(x - 2.0, y);
        node.outHandle = QPointF(x + 2.0, y);
        node.inHandleActive = index > 0;
        node.outHandleActive = index + 1 < 96;
        node.mode = VectorNodeMode::Smooth;
        shape.bezierPath.nodes.push_back(node);
    }
    shape.normalise();
    layer.vectorData.objects = {shape};
    layer.vectorData.normalise();
    QVERIFY(layer.vectorData.isSafe());

    const QSize size(512, 256);
    const QColorSpace colourSpace(QColorSpace::SRgb);
    const QImage full = VectorRasterizer::renderLayerRegion(
        layer, size, QRect(QPoint(), size), size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!full.isNull());

    QImage stitched(size, QImage::Format_RGBA8888);
    stitched.fill(Qt::transparent);
    for (int y = 0; y < size.height(); y += 64) {
        for (int x = 0; x < size.width(); x += 64) {
            const QRect region(x, y,
                               std::min(64, size.width() - x),
                               std::min(64, size.height() - y));
            const QImage tile = VectorRasterizer::renderLayerRegion(
                layer, size, region, size, QTransform(),
                QImage::Format_RGBA8888, colourSpace);
            QVERIFY(!tile.isNull());
            for (int row = 0; row < tile.height(); ++row) {
                std::memcpy(stitched.scanLine(region.y() + row)
                                + region.x() * 4,
                            tile.constScanLine(row),
                            static_cast<size_t>(tile.width()) * 4);
            }
        }
    }
    QVERIFY(exactImagesEqual(stitched, full));

    VectorShape &edited = layer.vectorData.objects.first();
    edited.bezierPath.nodes[48].anchor.ry() += 38.0;
    edited.bezierPath.nodes[48].inHandle.ry() += 38.0;
    edited.bezierPath.nodes[48].outHandle.ry() += 38.0;
    ++edited.revision;
    ++layer.revision;
    const QImage changed = VectorRasterizer::renderLayerRegion(
        layer, size, QRect(QPoint(), size), size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!changed.isNull());
    QVERIFY(!exactImagesEqual(changed, full));

    const QRect changedRegion(224, 64, 96, 160);
    const QImage changedTile = VectorRasterizer::renderLayerRegion(
        layer, size, changedRegion, size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!changedTile.isNull());
    for (int row = 0; row < changedTile.height(); ++row) {
        const uchar *expected = changed.constScanLine(changedRegion.y() + row)
            + changedRegion.x() * 4;
        QVERIFY(std::memcmp(changedTile.constScanLine(row), expected,
                            static_cast<size_t>(changedRegion.width()) * 4) == 0);
    }

    VectorRasterizer::clearCache();
}

void CoreTests::vectorRasterizerPreservesResolutionIndependenceAndBitDepth()
{
    VectorRasterizer::clearCache();
    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = QStringLiteral("Rasterisation Reference");
    VectorShape shape;
    shape.type = VectorShapeType::Rectangle;
    shape.bounds = QRectF(8.0, 10.0, 24.0, 18.0);
    shape.fill.colour = QColor::fromRgba64(
        QRgba64::fromRgba64(10000, 30000, 50000, 40000));
    layer.vectorData.objects = {shape};
    layer.vectorData.normalise();

    const QColorSpace space(QColorSpace::SRgb);
    const QSize documentSize(64, 64);
    const QRect fullRegion(QPoint(), documentSize);
    const QImage rgba8 = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA8888, space);
    QVERIFY(!rgba8.isNull());
    QCOMPARE(rgba8.pixelColor(15, 18).red(),
             shape.fill.colour.toRgb().red());
    QCOMPARE(rgba8.pixelColor(15, 18).alpha(),
             shape.fill.colour.toRgb().alpha());
    QCOMPARE(rgba8.pixelColor(2, 2).alpha(), 0);
    const qsizetype entriesAfterFirst = VectorRasterizer::cacheEntryCount();
    QVERIFY(entriesAfterFirst > 0);
    const QImage cached = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA8888, space);
    QVERIFY(exactImagesEqual(rgba8, cached));
    QCOMPARE(VectorRasterizer::cacheEntryCount(), entriesAfterFirst);

    const QImage rgba16 = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA64, space);
    QVERIFY(!rgba16.isNull());
    const QRgba64 interior = reinterpret_cast<const QRgba64 *>(
        rgba16.constScanLine(18))[15];
    const QRgba64 expected = shape.fill.colour.rgba64();
    QVERIFY(std::abs(int(interior.red()) - int(expected.red())) <= 2);
    QVERIFY(std::abs(int(interior.green()) - int(expected.green())) <= 2);
    QVERIFY(std::abs(int(interior.blue()) - int(expected.blue())) <= 2);
    QVERIFY(std::abs(int(interior.alpha()) - int(expected.alpha())) <= 2);

    layer.vectorData.objects.first().type = VectorShapeType::RoundedRectangle;
    layer.vectorData.objects.first().cornerRadii.setAll(7.0);
    ++layer.vectorData.objects.first().revision;
    const QImage rounded = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA8888, space);
    QVERIFY(rounded.pixelColor(8, 10).alpha() < 64);
    QVERIFY(rounded.pixelColor(20, 19).alpha() > 200);

    layer.vectorData.objects.first().type = VectorShapeType::Ellipse;
    ++layer.vectorData.objects.first().revision;
    const QImage ellipse = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA8888, space);
    QVERIFY(ellipse.pixelColor(8, 10).alpha() < 64);
    QVERIFY(ellipse.pixelColor(20, 19).alpha() > 200);

    layer.vectorData.objects.first().type = VectorShapeType::Rectangle;
    layer.vectorData.objects.first().fill.opacity = 0.0;
    ++layer.vectorData.objects.first().revision;
    const QImage transparent = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA64, space, false);
    QCOMPARE(reinterpret_cast<const QRgba64 *>(transparent.constScanLine(18))[15].alpha(),
             quint16(0));
    const QImage rgbReference = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA64, space, true);
    const QRgba64 hidden = reinterpret_cast<const QRgba64 *>(
        rgbReference.constScanLine(18))[15];
    QCOMPARE(hidden.red(), expected.red());
    QCOMPARE(hidden.green(), expected.green());
    QCOMPARE(hidden.blue(), expected.blue());
    QCOMPARE(hidden.alpha(), quint16(65535));

    const QImage halfSize = VectorRasterizer::renderLayerRegion(
        layer, QSize(32, 32), QRect(0, 0, 32, 32), documentSize, QTransform(),
        QImage::Format_RGBA8888, space, true);
    QVERIFY(halfSize.pixelColor(10, 9).alpha() > 200);
    QCOMPARE(VectorRasterizer::contentBounds(layer),
             layer.vectorData.contentBounds());

    std::atomic_bool cancelled {true};
    const QImage cancelledImage = VectorRasterizer::renderLayerRegion(
        layer, documentSize, fullRegion, documentSize, QTransform(),
        QImage::Format_RGBA8888, space, false, false, &cancelled);
    QVERIFY(cancelledImage.isNull());
    QVERIFY(VectorRasterizer::cacheBytes() > 0);
    VectorRasterizer::clearCache();
    QCOMPARE(VectorRasterizer::cacheEntryCount(), qsizetype(0));
}


void CoreTests::vectorRoundedCornersStayCircularAndRoundTripIndividually()
{
    VectorShape shape;
    shape.type = VectorShapeType::RoundedRectangle;
    shape.bounds = QRectF(0.0, 0.0, 100.0, 80.0);
    shape.cornerRadii = VectorCornerRadii {10.0, 20.0, 30.0, 15.0};
    shape.cornerRadiiLinked = false;
    shape.normalise();
    QVERIFY(shape.isSafe());

    QTransform nonUniformScale;
    nonUniformScale.scale(2.0, 1.0);
    const QPainterPath scaledPath = shape.pathForWorldTransform(nonUniformScale);
    QVERIFY(!scaledPath.isEmpty());
    QVERIFY(scaledPath.elementCount() >= 2);
    const QPainterPath::Element start = scaledPath.elementAt(0);
    const QPainterPath::Element topEdgeEnd = scaledPath.elementAt(1);
    QVERIFY(std::abs(start.x - 10.0) <= 1.0e-9);
    QVERIFY(std::abs(start.y) <= 1.0e-9);
    // The top-right radius remains 20 document pixels. A naively transformed
    // local path would stretch it to 40 and end this edge at x=160.
    QVERIFY(std::abs(topEdgeEnd.x - 180.0) <= 1.0e-9);
    QVERIFY(std::abs(topEdgeEnd.y) <= 1.0e-9);
    QCOMPARE(scaledPath.boundingRect(), QRectF(0.0, 0.0, 200.0, 80.0));

    QTransform uniformScale;
    uniformScale.scale(2.0, 2.0);
    const QPainterPath uniformlyScaled = shape.pathForWorldTransform(uniformScale);
    QVERIFY(uniformlyScaled.elementCount() >= 2);
    // Radius values are document pixels and therefore do not double under a
    // uniform layer transform either.
    QVERIFY(std::abs(uniformlyScaled.elementAt(0).x - 10.0) <= 1.0e-9);
    QVERIFY(std::abs(uniformlyScaled.elementAt(1).x - 180.0) <= 1.0e-9);

    QCOMPARE(shape.cornerRadii.minimumSize(), QSizeF(45.0, 50.0));
    QVERIFY(shape.cornerRadiiFitWorldTransform(nonUniformScale));
    QTransform tooSmall;
    tooSmall.scale(0.4, 0.4);
    QVERIFY(!shape.cornerRadiiFitWorldTransform(tooSmall));

    VectorShape enlargedBody;
    enlargedBody.type = VectorShapeType::RoundedRectangle;
    enlargedBody.bounds = QRectF(0.0, 0.0, 20.0, 20.0);
    enlargedBody.cornerRadii.setAll(15.0);
    enlargedBody.cornerRadiiLinked = true;
    enlargedBody.normalise();
    // A radius may legitimately exceed half the local bounds when the layer
    // transform enlarges the body. Normalisation must retain the document-pixel
    // value rather than baking local-scale semantics back into the payload.
    QCOMPARE(enlargedBody.cornerRadii.topLeft, 15.0);
    QVERIFY(!enlargedBody.cornerRadiiFitWorldTransform(QTransform()));
    QTransform enlargedWorld;
    enlargedWorld.scale(2.0, 2.0);
    QVERIFY(enlargedBody.cornerRadiiFitWorldTransform(enlargedWorld));
    const QPainterPath enlargedPath = enlargedBody.pathForWorldTransform(enlargedWorld);
    QVERIFY(std::abs(enlargedPath.elementAt(0).x - 15.0) <= 1.0e-9);

    bool encodedOk = false;
    const QJsonObject encoded = shape.toJson(&encodedOk);
    QVERIFY(encodedOk);
    QCOMPARE(encoded.value(QStringLiteral("cornerRadiiLinked")).toBool(), false);
    const QJsonObject radii = encoded.value(QStringLiteral("cornerRadii")).toObject();
    QCOMPARE(radii.value(QStringLiteral("topLeft")).toDouble(), 10.0);
    QCOMPARE(radii.value(QStringLiteral("topRight")).toDouble(), 20.0);
    QCOMPARE(radii.value(QStringLiteral("bottomRight")).toDouble(), 30.0);
    QCOMPARE(radii.value(QStringLiteral("bottomLeft")).toDouble(), 15.0);

    bool decodedOk = false;
    const VectorShape decoded = VectorShape::fromJson(encoded, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(decoded.cornerRadii, shape.cornerRadii);
    QVERIFY(!decoded.cornerRadiiLinked);

    QJsonObject legacy = encoded;
    legacy.remove(QStringLiteral("cornerRadii"));
    legacy.remove(QStringLiteral("cornerRadiiLinked"));
    legacy.insert(QStringLiteral("cornerRadius"), 7.0);
    const VectorShape migrated = VectorShape::fromJson(legacy, &decodedOk);
    QVERIFY(decodedOk);
    QVERIFY(migrated.cornerRadiiLinked);
    QVERIFY(migrated.cornerRadii.allEqual());
    QCOMPARE(migrated.cornerRadii.topLeft, 7.0);
}


void CoreTests::vectorShapeConversionPreservesVisibleAppearanceAndRoundTrips()
{
    const QVector<VectorShapeType> primitiveTypes {
        VectorShapeType::Rectangle,
        VectorShapeType::RoundedRectangle,
        VectorShapeType::Ellipse,
        VectorShapeType::Polygon,
        VectorShapeType::Star,
        VectorShapeType::Arrow,
        VectorShapeType::Line,
    };
    const QSize documentSize(256, 224);
    const QRect fullRegion(QPoint(), documentSize);
    const QColorSpace colourSpace(QColorSpace::SRgb);

    for (const VectorShapeType type : primitiveTypes) {
        VectorShape shape;
        shape.type = type;
        shape.bounds = QRectF(34.0, 28.0, 92.0, 74.0);
        shape.lineStart = QPointF(28.0, 42.0);
        shape.lineEnd = QPointF(137.0, 111.0);
        shape.polygonSides = 7;
        shape.starInnerRatio = 0.37;
        shape.vertexRotationDegrees = -71.0;
        shape.arrowHeadLengthRatio = 0.43;
        shape.arrowShaftWidthRatio = 0.31;
        shape.cornerRadii = VectorCornerRadii {9.0, 17.0, 13.0, 21.0};
        shape.cornerRadiiLinked = false;
        shape.fill.enabled = type != VectorShapeType::Line;
        shape.fill.colour = QColor(31, 148, 219, 187);
        shape.fill.opacity = 0.73;
        shape.stroke.enabled = true;
        shape.stroke.colour = QColor(231, 68, 41, 211);
        shape.stroke.opacity = 0.82;
        shape.stroke.width = 6.25;
        shape.stroke.alignment = type == VectorShapeType::Line
            ? VectorStrokeAlignment::Centre : VectorStrokeAlignment::Outside;
        shape.stroke.cap = VectorStrokeCap::Round;
        shape.stroke.join = VectorStrokeJoin::Bevel;
        shape.stroke.miterLimit = 9.5;
        shape.stroke.pattern = VectorStrokePattern::Dashed;
        shape.stroke.dashLength = 11.0;
        shape.stroke.gapLength = 6.5;
        shape.stroke.dashOffset = 3.25;
        shape.transform.translate(7.0, 5.0);
        shape.transform.scale(1.45, 0.82);
        shape.normalise();
        QVERIFY2(shape.isSafe(), qPrintable(vectorShapeTypeDisplayName(type)));

        LayerNode beforeLayer;
        beforeLayer.type = LayerType::Vector;
        beforeLayer.name = QStringLiteral("Convertible Shape");
        beforeLayer.opacity = 0.91;
        beforeLayer.blendMode = BlendMode::Copy;
        beforeLayer.transform.translate(18.0, 13.0);
        beforeLayer.vectorData.objects = {shape};
        beforeLayer.vectorData.normalise();
        QVERIFY(beforeLayer.vectorData.isSafe());

        const QPainterPath visibleBefore = shape.pathForWorldTransform(
            beforeLayer.transform);
        const QImage beforeImage = VectorRasterizer::renderLayerRegion(
            beforeLayer, documentSize, fullRegion, documentSize, QTransform(),
            QImage::Format_RGBA8888, colourSpace);
        QVERIFY(!beforeImage.isNull());

        const QUuid originalId = shape.id;
        const VectorFill originalFill = shape.fill;
        const VectorStroke originalStroke = shape.stroke;
        const QTransform originalTransform = shape.transform;
        const quint64 originalRevision = shape.revision;
        QVERIFY2(shape.convertToPath(beforeLayer.transform),
                 qPrintable(vectorShapeTypeDisplayName(type)));
        QCOMPARE(shape.type, VectorShapeType::Path);
        QCOMPARE(shape.id, originalId);
        QCOMPARE(shape.fill, originalFill);
        QCOMPARE(shape.stroke, originalStroke);
        QVERIFY(transformsClose(shape.transform, originalTransform));
        QVERIFY(shape.revision > originalRevision);
        QCOMPARE(shape.bezierPath.closed, type != VectorShapeType::Line);
        QVERIFY(shape.bezierPath.nodes.size() >= 2);
        QVERIFY(shape.bezierPath.isSafe());
        QSet<QUuid> nodeIds;
        for (const VectorPathNode &node : shape.bezierPath.nodes) {
            QVERIFY(!node.id.isNull());
            QVERIFY(!nodeIds.contains(node.id));
            nodeIds.insert(node.id);
        }

        const QPainterPath visibleAfter = shape.pathForWorldTransform(
            beforeLayer.transform);
        const QRectF beforeBounds = visibleBefore.boundingRect();
        const QRectF afterBounds = visibleAfter.boundingRect();
        QVERIFY(std::abs(beforeBounds.left() - afterBounds.left()) <= 1.0e-6);
        QVERIFY(std::abs(beforeBounds.top() - afterBounds.top()) <= 1.0e-6);
        QVERIFY(std::abs(beforeBounds.right() - afterBounds.right()) <= 1.0e-6);
        QVERIFY(std::abs(beforeBounds.bottom() - afterBounds.bottom()) <= 1.0e-6);

        LayerNode afterLayer = beforeLayer;
        afterLayer.vectorData.objects = {shape};
        afterLayer.vectorData.normalise();
        const QImage afterImage = VectorRasterizer::renderLayerRegion(
            afterLayer, documentSize, fullRegion, documentSize, QTransform(),
            QImage::Format_RGBA8888, colourSpace);
        QVERIFY(!afterImage.isNull());
        QVERIFY2(imagesWithinChannelTolerance(beforeImage, afterImage, 2),
                 qPrintable(vectorShapeTypeDisplayName(type)));

        bool encodedOk = false;
        const QJsonObject encoded = shape.toJson(&encodedOk);
        QVERIFY(encodedOk);
        bool decodedOk = false;
        const VectorShape decoded = VectorShape::fromJson(encoded, &decodedOk);
        QVERIFY(decodedOk);
        QCOMPARE(decoded, shape);
    }

    QTemporaryDir projectDirectory;
    QVERIFY(projectDirectory.isValid());
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Converted Path Project");
    settings.pixelSize = QSize(192, 144);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString projectError;
    QVERIFY2(document.createNewDocument(settings, &projectError),
             qPrintable(projectError));
    const QUuid convertedLayerId = document.addVectorShape(
        VectorShapeType::RoundedRectangle,
        QRectF(22.0, 19.0, 88.0, 63.0),
        QColor(40, 180, 220, 170), {}, 14.0);
    QVERIFY(!convertedLayerId.isNull());
    LayerNode convertedLayer = document.layerById(convertedLayerId);
    convertedLayer.opacity = 0.76;
    convertedLayer.transform.translate(13.0, 9.0);
    convertedLayer.transform.scale(1.35, 0.78);
    VectorShape &projectShape = convertedLayer.vectorData.objects.first();
    projectShape.cornerRadii = VectorCornerRadii {6.0, 15.0, 11.0, 19.0};
    projectShape.cornerRadiiLinked = false;
    projectShape.stroke.enabled = true;
    projectShape.stroke.width = 5.0;
    projectShape.stroke.pattern = VectorStrokePattern::Dashed;
    projectShape.stroke.dashLength = 9.0;
    projectShape.stroke.gapLength = 4.0;
    projectShape.normalise();
    convertedLayer.vectorData.normalise();
    QVERIFY(document.replaceLayer(convertedLayerId, convertedLayer));
    convertedLayer = document.layerById(convertedLayerId);
    QVERIFY(convertedLayer.vectorData.objects.first().convertToPath(
        document.layerWorldTransform(convertedLayerId)));
    convertedLayer.vectorData.normalise();
    QVERIFY(document.replaceLayer(convertedLayerId, convertedLayer));
    const LayerNode expectedConvertedLayer = document.layerById(convertedLayerId);
    const QString projectPath = projectDirectory.filePath(
        QStringLiteral("converted-path.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &projectError),
             qPrintable(projectError));
    PhotoDocument restoredDocument;
    QVERIFY2(restoredDocument.loadProject(projectPath, &projectError),
             qPrintable(projectError));
    QCOMPARE(restoredDocument.layerById(convertedLayerId), expectedConvertedLayer);

    VectorShape invalidRounded;
    invalidRounded.type = VectorShapeType::RoundedRectangle;
    invalidRounded.bounds = QRectF(0.0, 0.0, 80.0, 50.0);
    invalidRounded.cornerRadii.setAll(10.0);
    invalidRounded.normalise();
    QTransform singular;
    singular.scale(0.0, 1.0);
    QVERIFY(!invalidRounded.convertToPath(singular));
    QCOMPARE(invalidRounded.type, VectorShapeType::RoundedRectangle);

    VectorShape existingPath;
    existingPath.type = VectorShapeType::Path;
    VectorPathNode first;
    first.anchor = QPointF(2.0, 3.0);
    VectorPathNode second;
    second.anchor = QPointF(9.0, 8.0);
    existingPath.bezierPath.nodes = {first, second};
    existingPath.normalise();
    const VectorShape unchanged = existingPath;
    QVERIFY(existingPath.convertToPath());
    QCOMPARE(existingPath, unchanged);
}


void CoreTests::rasterLayerMergePreservesIsolatedCompositeAndHiddenRgb()
{
    NewDocumentSettings settings;
    settings.pixelSize = QSize(64, 48);
    settings.backgroundColour = QColor(Qt::white);
    PhotoDocument document;
    QString error;
    QVERIFY(document.createNewDocument(settings, &error));

    const QUuid lowerId = document.addRasterLayer();
    const QUuid upperId = document.addRasterLayer();
    QVERIFY(!lowerId.isNull());
    QVERIFY(!upperId.isNull());

    QImage lower(settings.pixelSize, QImage::Format_RGBA8888);
    lower.fill(Qt::transparent);
    lower.setColorSpace(document.sourceImage().colorSpace());
    // Keep meaningful hidden RGB just beyond the current canvas so Merge
    // Layers also proves it does not silently clip revealable raster storage.
    lower.setPixelColor(3, 4, QColor(12, 240, 33, 0));
    for (int y = 10; y < 34; ++y) {
        for (int x = 8; x < 38; ++x) {
            lower.setPixelColor(x, y, QColor(25, 90, 220, 210));
        }
    }
    QImage upper(settings.pixelSize, QImage::Format_RGBA8888);
    upper.fill(Qt::transparent);
    upper.setColorSpace(document.sourceImage().colorSpace());
    for (int y = 5; y < 28; ++y) {
        for (int x = 22; x < 55; ++x) {
            upper.setPixelColor(x, y, QColor(235, 45, 20, 160));
        }
    }
    QVERIFY(document.updateLayer(lowerId, [&](LayerNode &layer) {
        layer.rasterImage = lower;
        layer.rasterReferenceSize = lower.size();
        layer.rasterReferenceOrigin = QPointF(-6.0, -5.0);
        layer.transform = QTransform::fromTranslate(2.0, 1.0);
    }));
    QVERIFY(document.updateLayer(upperId, [&](LayerNode &layer) {
        layer.rasterImage = upper;
        layer.rasterReferenceSize = upper.size();
        layer.rasterReferenceOrigin = QPointF();
        layer.opacity = 0.7;
        layer.blendMode = BlendMode::Screen;
    }));
    const QUuid groupId = document.groupLayers(
        {upperId, lowerId}, QStringLiteral("Raster Pair"));
    QVERIFY(!groupId.isNull());
    QVERIFY(document.updateLayer(groupId, [](LayerNode &group) {
        group.transform = QTransform::fromTranslate(-3.0, -2.0);
    }));

    QImage transparent(settings.pixelSize, QImage::Format_RGBA8888);
    transparent.fill(Qt::transparent);
    transparent.setColorSpace(document.sourceImage().colorSpace());
    QVector<LayerNode> isolated;
    for (const QUuid &id : QVector<QUuid>{upperId, lowerId}) {
        LayerNode layer = document.layerById(id);
        layer.transform = document.layerWorldTransform(id);
        isolated.push_back(layer);
    }
    const QImage before = ImageProcessor::renderPreservingHiddenRgb(
        transparent, isolated, nullptr, settings.pixelSize);
    QVERIFY(!before.isNull());

    const LayerMergePlan plan = LayerMergeOperations::analyse(
        document, {upperId, lowerId}, &error);
    QVERIFY2(plan.isValid(), qPrintable(error));
    QCOMPARE(plan.kind, LayerMergeKind::Raster);
    QCOMPARE(plan.parentId, groupId);
    LayerNode merged;
    QVERIFY2(LayerMergeOperations::buildMergedLayer(
                 document, plan, &merged, &error), qPrintable(error));
    QCOMPARE(merged.type, LayerType::Raster);
    QCOMPARE(merged.id, upperId);
    QCOMPARE(merged.opacity, 1.0);
    QCOMPARE(merged.blendMode, BlendMode::Copy);
    QVERIFY(!merged.hasMask());

    QVector<LayerNode> mergedTree;
    QVERIFY2(LayerMergeOperations::replacePlannedRange(
                 document.layers(), plan, merged, &mergedTree, &error), qPrintable(error));
    QCOMPARE(mergedTree.at(0).id, groupId);
    QCOMPARE(mergedTree.at(0).children.size(), 1);
    QVector<LayerNode> mergedOnly {mergedTree.at(0)};
    const QImage after = ImageProcessor::renderPreservingHiddenRgb(
        transparent, mergedOnly, nullptr, settings.pixelSize);
    QVERIFY(!after.isNull());
    QVERIFY2(exactImagesEqual(before, after), "Merged raster output changed the isolated selected-layer composite.");

    bool foundOffCanvasHiddenRgb = false;
    for (int y = 0; y < merged.rasterImage.height() && !foundOffCanvasHiddenRgb; ++y) {
        for (int x = 0; x < merged.rasterImage.width(); ++x) {
            const QColor pixel = merged.rasterImage.pixelColor(x, y);
            const QPointF documentPoint = merged.rasterReferenceOrigin
                + QPointF(x, y);
            if (documentPoint.x() < 0.0 && pixel.alpha() == 0
                && pixel.red() == 12 && pixel.green() == 240
                && pixel.blue() == 33) {
                foundOffCanvasHiddenRgb = true;
                break;
            }
        }
    }
    QVERIFY(foundOffCanvasHiddenRgb);
    QVERIFY(merged.rasterReferenceOrigin.x() < 0.0);

    NewDocumentSettings highSettings;
    highSettings.pixelSize = QSize(24, 18);
    highSettings.bitDepth = 16;
    highSettings.backgroundColour = QColor(Qt::transparent);
    PhotoDocument highDocument;
    QVERIFY(highDocument.createNewDocument(highSettings, &error));
    const QUuid highLowerId = highDocument.addRasterLayer();
    const QUuid highUpperId = highDocument.addRasterLayer();
    QVERIFY(highDocument.updateLayer(highLowerId, [](LayerNode &layer) {
        QImage pixels(QSize(24, 18), QImage::Format_RGBA64);
        pixels.fill(QColor::fromRgba64(QRgba64::fromRgba64(12000, 28000, 51000, 42000)));
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = pixels.size();
    }));
    QVERIFY(highDocument.updateLayer(highUpperId, [](LayerNode &layer) {
        QImage pixels(QSize(24, 18), QImage::Format_RGBA64);
        pixels.fill(QColor::fromRgba64(QRgba64::fromRgba64(60000, 9000, 22000, 17000)));
        layer.rasterImage = pixels;
        layer.rasterReferenceSize = pixels.size();
        layer.opacity = 0.55;
    }));
    const LayerMergePlan highPlan = LayerMergeOperations::analyse(
        highDocument, {highUpperId, highLowerId}, &error);
    QVERIFY2(highPlan.isValid(), qPrintable(error));
    LayerNode highMerged;
    QVERIFY2(LayerMergeOperations::buildMergedLayer(
                 highDocument, highPlan, &highMerged, &error), qPrintable(error));
    QCOMPARE(highMerged.rasterImage.format(), QImage::Format_RGBA64);
}

void CoreTests::vectorLayerMergeConvertsShapesAndPreservesAppearance()
{
    NewDocumentSettings settings;
    settings.pixelSize = QSize(240, 180);
    PhotoDocument document;
    QString error;
    QVERIFY(document.createNewDocument(settings, &error));

    const QUuid lowerId = document.addVectorShape(
        VectorShapeType::RoundedRectangle,
        QRectF(18.0, 22.0, 112.0, 76.0),
        QColor(40, 170, 230));
    const QUuid upperId = document.addVectorShape(
        VectorShapeType::Ellipse,
        QRectF(75.0, 44.0, 96.0, 88.0),
        QColor(240, 70, 60));
    QVERIFY(!lowerId.isNull());
    QVERIFY(!upperId.isNull());
    QVERIFY(document.updateLayer(lowerId, [](LayerNode &layer) {
        layer.transform = QTransform::fromTranslate(7.0, 4.0)
            * QTransform::fromScale(1.15, 0.85);
        layer.vectorData.objects[0].cornerRadii.setAll(18.0);
        layer.vectorData.objects[0].stroke.enabled = true;
        layer.vectorData.objects[0].stroke.colour = QColor(15, 25, 35);
        layer.vectorData.objects[0].stroke.width = 5.0;
        layer.vectorData.objects[0].normalise();
        layer.vectorData.normalise();
    }));
    QVERIFY(document.updateLayer(upperId, [](LayerNode &layer) {
        layer.transform = QTransform::fromTranslate(-4.0, 9.0);
        layer.vectorData.objects[0].fill.opacity = 0.72;
        layer.vectorData.objects[0].stroke.enabled = true;
        layer.vectorData.objects[0].stroke.colour = QColor(250, 245, 40);
        layer.vectorData.objects[0].stroke.width = 3.0;
        layer.vectorData.objects[0].normalise();
        layer.vectorData.normalise();
    }));

    const QUuid groupId = document.groupLayers(
        {upperId, lowerId}, QStringLiteral("Vector Pair"));
    QVERIFY(!groupId.isNull());
    QVERIFY(document.updateLayer(groupId, [](LayerNode &group) {
        group.transform = QTransform::fromTranslate(13.0, -6.0)
            * QTransform::fromScale(0.9, 1.1);
    }));

    QImage transparent(settings.pixelSize, QImage::Format_RGBA8888);
    transparent.fill(Qt::transparent);
    transparent.setColorSpace(document.sourceImage().colorSpace());
    const QImage before = ImageProcessor::render(
        transparent, document.layers(), nullptr, settings.pixelSize);
    QVERIFY(!before.isNull());

    const LayerMergePlan plan = LayerMergeOperations::analyse(
        document, {upperId, lowerId}, &error);
    QVERIFY2(plan.isValid(), qPrintable(error));
    QCOMPARE(plan.kind, LayerMergeKind::Vector);
    QCOMPARE(plan.parentId, groupId);
    LayerNode merged;
    QVERIFY2(LayerMergeOperations::buildMergedLayer(
                 document, plan, &merged, &error), qPrintable(error));
    QCOMPARE(merged.type, LayerType::Vector);
    QCOMPARE(merged.id, upperId);
    QCOMPARE(merged.vectorData.objects.size(), 2);
    for (const VectorShape &shape : merged.vectorData.objects) {
        QCOMPARE(shape.type, VectorShapeType::Path);
        QVERIFY(shape.transform.isIdentity());
        QVERIFY(shape.isSafe());
    }

    QVector<LayerNode> mergedTree;
    QVERIFY2(LayerMergeOperations::replacePlannedRange(
                 document.layers(), plan, merged, &mergedTree, &error), qPrintable(error));
    const QImage after = ImageProcessor::render(
        transparent, mergedTree, nullptr, settings.pixelSize);
    QVERIFY(!after.isNull());
    QVERIFY2(imagesWithinChannelTolerance(before, after, 1),
             "Merged editable paths changed the visible vector appearance.");

    QCOMPARE(mergedTree.size(), 2); // Transformed group plus original Background.
    QCOMPARE(mergedTree.at(0).id, groupId);
    QCOMPARE(mergedTree.at(0).children.size(), 1);
    QCOMPARE(mergedTree.at(0).children.constFirst().id, upperId);
    QCOMPARE(mergedTree.at(0).children.constFirst().type, LayerType::Vector);
}

void CoreTests::milestoneIntegrationRoundTripPreservesFillGradientAndMerge()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString projectPath = directory.filePath(
        QStringLiteral("vector-fill-gradient-integration.vfxphoto"));

    NewDocumentSettings settings;
    settings.pixelSize = QSize(80, 60);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(Qt::transparent);
    PhotoDocument document;
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const QUuid lowerRasterId = document.addRasterLayer();
    const QUuid upperRasterId = document.addRasterLayer();
    QVERIFY(!lowerRasterId.isNull());
    QVERIFY(!upperRasterId.isNull());

    QImage lower(settings.pixelSize, QImage::Format_RGBA64);
    lower.fill(QColor::fromRgba64(QRgba64::fromRgba64(9000, 17000, 25000, 0)));
    QImage fillCoverage(settings.pixelSize, QImage::Format_Grayscale8);
    fillCoverage.fill(0);
    for (int y = 9; y < 49; ++y) {
        uchar *row = fillCoverage.scanLine(y);
        for (int x = 7; x < 54; ++x) row[x] = 255;
    }
    const QColor fillColour = QColor::fromRgba64(
        QRgba64::fromRgba64(51000, 12000, 43000, 47000));
    const FillApplyResult filled = applyFillCoverageCpu(
        lower, fillCoverage, FillTarget::RasterPixels, -1, fillColour, false);
    QVERIFY2(filled.succeeded(), qPrintable(filled.error));
    QVERIFY(filled.changed());

    QImage upper(settings.pixelSize, QImage::Format_RGBA64);
    upper.fill(QColor::fromRgba64(QRgba64::fromRgba64(6000, 30000, 42000, 0)));
    QImage gradientCoverage(settings.pixelSize, QImage::Format_Grayscale8);
    gradientCoverage.fill(255);
    GradientApplyRequest gradientRequest;
    gradientRequest.sourceImage = upper;
    gradientRequest.selectionCoverage = gradientCoverage;
    gradientRequest.target = FillTarget::RasterPixels;
    gradientRequest.start = QPointF(10.5, 12.5);
    gradientRequest.end = QPointF(70.5, 47.5);
    gradientRequest.type = RasterGradientType::Diamond;
    gradientRequest.startColour = QColor::fromRgba64(
        QRgba64::fromRgba64(4000, 55000, 21000, 52000));
    gradientRequest.endColour = QColor::fromRgba64(
        QRgba64::fromRgba64(62000, 8000, 50000, 9000));
    const GradientApplyResult gradient = applyGradientCpu(gradientRequest);
    QVERIFY2(gradient.succeeded(), qPrintable(gradient.error));
    QVERIFY(gradient.changed());

    QVERIFY(document.updateLayer(lowerRasterId, [&](LayerNode &layer) {
        layer.rasterImage = filled.image;
        layer.rasterReferenceSize = filled.image.size();
    }));
    QVERIFY(document.updateLayer(upperRasterId, [&](LayerNode &layer) {
        layer.rasterImage = gradient.image;
        layer.rasterReferenceSize = gradient.image.size();
        layer.opacity = 0.68;
        layer.blendMode = BlendMode::Screen;
        QImage mask(settings.pixelSize, QImage::Format_Grayscale8);
        mask.fill(255);
        for (int y = 0; y < mask.height(); ++y) {
            uchar *row = mask.scanLine(y);
            for (int x = 0; x < mask.width(); ++x) {
                row[x] = static_cast<uchar>(std::clamp(x * 255 / std::max(1, mask.width() - 1), 0, 255));
            }
        }
        layer.maskImage = mask;
        layer.maskReferenceSize = mask.size();
    }));

    LayerMergePlan rasterPlan = LayerMergeOperations::analyse(
        document, {upperRasterId, lowerRasterId}, &error);
    QVERIFY2(rasterPlan.isValid(), qPrintable(error));
    LayerNode mergedRaster;
    QVERIFY2(LayerMergeOperations::buildMergedLayer(
                 document, rasterPlan, &mergedRaster, &error), qPrintable(error));
    QVector<LayerNode> rasterMergedTree;
    QVERIFY2(LayerMergeOperations::replacePlannedRange(
                 document.layers(), rasterPlan, mergedRaster,
                 &rasterMergedTree, &error), qPrintable(error));
    QVERIFY(document.replaceLayerTree(rasterMergedTree));

    const QUuid lowerVectorId = document.addVectorShape(
        VectorShapeType::RoundedRectangle,
        QRectF(9.0, 8.0, 36.0, 26.0), QColor(55, 140, 230, 210), {}, 7.0);
    const QUuid upperVectorId = document.addVectorShape(
        VectorShapeType::Arrow,
        QRectF(30.0, 25.0, 40.0, 22.0), QColor(230, 120, 35, 190));
    QVERIFY(!lowerVectorId.isNull());
    QVERIFY(!upperVectorId.isNull());
    QVERIFY(document.updateLayer(lowerVectorId, [](LayerNode &layer) {
        VectorShape &shape = layer.vectorData.objects.first();
        shape.stroke.enabled = true;
        shape.stroke.width = 3.0;
        shape.stroke.colour = QColor(20, 240, 80, 255);
        shape.normalise();
        layer.vectorData.normalise();
    }));
    QVERIFY(document.updateLayer(upperVectorId, [](LayerNode &layer) {
        layer.transform = QTransform::fromTranslate(2.0, -1.0);
        VectorShape &shape = layer.vectorData.objects.first();
        shape.stroke.enabled = true;
        shape.stroke.width = 2.0;
        shape.stroke.colour = QColor(245, 230, 40, 255);
        shape.normalise();
        layer.vectorData.normalise();
    }));

    LayerMergePlan vectorPlan = LayerMergeOperations::analyse(
        document, {upperVectorId, lowerVectorId}, &error);
    QVERIFY2(vectorPlan.isValid(), qPrintable(error));
    LayerNode mergedVector;
    QVERIFY2(LayerMergeOperations::buildMergedLayer(
                 document, vectorPlan, &mergedVector, &error), qPrintable(error));
    QVector<LayerNode> finalTree;
    QVERIFY2(LayerMergeOperations::replacePlannedRange(
                 document.layers(), vectorPlan, mergedVector,
                 &finalTree, &error), qPrintable(error));
    QVERIFY(document.replaceLayerTree(finalTree));

    document.selectionMask().selectNone();
    QVERIFY(document.selectionMask().setCoverageRect(QRect(12, 10, 43, 31), 173));
    document.setGuides({11.0, 37.0}, {8.0, 52.0});

    const QImage before = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr,
        document.sourceImage().size());
    QVERIFY(!before.isNull());
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QCOMPARE(restored.sourceImage().format(), QImage::Format_RGBA64);
    QCOMPARE(restored.horizontalGuides(), QVector<double>({11.0, 37.0}));
    QCOMPARE(restored.verticalGuides(), QVector<double>({8.0, 52.0}));
    QVERIFY(restored.selectionMask().isActive());
    QCOMPARE(restored.selectionMask().coverageAt(20, 20), static_cast<quint8>(173));

    const LayerNode restoredRaster = restored.layerById(upperRasterId);
    QCOMPARE(restoredRaster.type, LayerType::Raster);
    QCOMPARE(restoredRaster.rasterImage.format(), QImage::Format_RGBA64);
    QVERIFY(!restoredRaster.hasMask());
    const LayerNode restoredVector = restored.layerById(upperVectorId);
    QCOMPARE(restoredVector.type, LayerType::Vector);
    QCOMPARE(restoredVector.vectorData.objects.size(), 2);
    for (const VectorShape &shape : restoredVector.vectorData.objects) {
        QCOMPARE(shape.type, VectorShapeType::Path);
        QVERIFY(shape.isSafe());
    }

    const QImage after = ImageProcessor::renderPreservingHiddenRgb(
        restored.sourceImage(), restored.layers(), nullptr,
        restored.sourceImage().size());
    QVERIFY(!after.isNull());
    QVERIFY2(exactImagesEqual(before, after),
             "Save/reopen changed the integrated Fill, Gradient or merged-layer result.");
}

void CoreTests::expandedVectorStrokesPreserveVisibleGeometryAndRoundTrip()
{
    const QSize documentSize(320, 240);
    const QRect region(QPoint(), documentSize);
    const QColorSpace colourSpace(QColorSpace::SRgb);
    const auto coverageMatches = [](const QPainterPath &left,
                                    const QPainterPath &right) {
        const QRectF bounds = left.boundingRect().united(right.boundingRect());
        if (bounds.isEmpty()) return false;
        for (int y = 0; y < 97; ++y) {
            for (int x = 0; x < 97; ++x) {
                const QPointF point(
                    bounds.left() + bounds.width() * (x + 0.5) / 97.0,
                    bounds.top() + bounds.height() * (y + 0.5) / 97.0);
                if (left.contains(point) != right.contains(point)) return false;
            }
        }
        return true;
    };

    QVector<VectorShape> cases;

    VectorShape openDashed;
    openDashed.type = VectorShapeType::Line;
    openDashed.lineStart = QPointF(28.0, 44.0);
    openDashed.lineEnd = QPointF(238.0, 122.0);
    openDashed.fill.enabled = false;
    openDashed.stroke.enabled = true;
    openDashed.stroke.colour = QColor(31, 145, 224, 207);
    openDashed.stroke.opacity = 0.73;
    openDashed.stroke.width = 17.0;
    openDashed.stroke.cap = VectorStrokeCap::Round;
    openDashed.stroke.join = VectorStrokeJoin::Round;
    openDashed.stroke.pattern = VectorStrokePattern::Dashed;
    openDashed.stroke.dashLength = 27.0;
    openDashed.stroke.gapLength = 11.0;
    openDashed.stroke.dashOffset = 6.0;
    openDashed.transform.rotate(-11.0);
    openDashed.normalise();
    cases.push_back(openDashed);

    VectorShape dashedRectangle;
    dashedRectangle.type = VectorShapeType::Rectangle;
    dashedRectangle.bounds = QRectF(38.0, 32.0, 188.0, 118.0);
    dashedRectangle.fill.enabled = false;
    dashedRectangle.stroke.enabled = true;
    dashedRectangle.stroke.colour = QColor(72, 71, 132, 255);
    dashedRectangle.stroke.width = 10.0;
    dashedRectangle.stroke.cap = VectorStrokeCap::Round;
    dashedRectangle.stroke.join = VectorStrokeJoin::Miter;
    dashedRectangle.stroke.pattern = VectorStrokePattern::Dashed;
    dashedRectangle.stroke.dashLength = 21.0;
    dashedRectangle.stroke.gapLength = 12.0;
    dashedRectangle.normalise();
    cases.push_back(dashedRectangle);

    VectorShape insideRectangle;
    insideRectangle.type = VectorShapeType::Rectangle;
    insideRectangle.bounds = QRectF(62.0, 48.0, 132.0, 94.0);
    insideRectangle.fill.enabled = false;
    insideRectangle.stroke.enabled = true;
    insideRectangle.stroke.colour = QColor(232, 84, 38, 191);
    insideRectangle.stroke.opacity = 0.64;
    insideRectangle.stroke.width = 19.0;
    insideRectangle.stroke.alignment = VectorStrokeAlignment::Inside;
    insideRectangle.stroke.join = VectorStrokeJoin::Miter;
    insideRectangle.stroke.miterLimit = 2.5;
    insideRectangle.transform.shear(0.18, -0.07);
    insideRectangle.normalise();
    cases.push_back(insideRectangle);

    VectorShape outsideStar;
    outsideStar.type = VectorShapeType::Star;
    outsideStar.bounds = QRectF(74.0, 38.0, 116.0, 116.0);
    outsideStar.polygonSides = 7;
    outsideStar.starInnerRatio = 0.42;
    outsideStar.vertexRotationDegrees = -82.0;
    outsideStar.fill.enabled = false;
    outsideStar.stroke.enabled = true;
    outsideStar.stroke.colour = QColor(106, 214, 91, 223);
    outsideStar.stroke.opacity = 0.81;
    outsideStar.stroke.width = 13.0;
    outsideStar.stroke.alignment = VectorStrokeAlignment::Outside;
    outsideStar.stroke.join = VectorStrokeJoin::Bevel;
    outsideStar.normalise();
    cases.push_back(outsideStar);

    VectorShape centreEllipse;
    centreEllipse.type = VectorShapeType::Ellipse;
    centreEllipse.bounds = QRectF(70.0, 45.0, 142.0, 96.0);
    centreEllipse.fill.enabled = false;
    centreEllipse.stroke.enabled = true;
    centreEllipse.stroke.colour = QColor(176, 72, 218, 215);
    centreEllipse.stroke.opacity = 0.69;
    centreEllipse.stroke.width = 21.0;
    centreEllipse.stroke.alignment = VectorStrokeAlignment::Centre;
    centreEllipse.normalise();
    cases.push_back(centreEllipse);

    QTransform layerWorld;
    layerWorld.translate(21.0, 17.0);
    layerWorld.scale(1.08, 0.91);

    for (const VectorShape &source : std::as_const(cases)) {
        QVERIFY(source.isSafe());
        VectorShape expanded;
        QVERIFY2(source.expandedStrokePath(layerWorld, &expanded),
                 qPrintable(vectorShapeTypeDisplayName(source.type)));
        QCOMPARE(expanded.type, VectorShapeType::Path);
        QVERIFY(expanded.bezierPath.closed);
        QVERIFY(expanded.bezierPath.nodes.size() >= 3);
        QVERIFY(expanded.fill.enabled);
        const VectorPathFillRule expectedFillRule =
            source.strokePathForWorldTransform(layerWorld).fillRule()
                    == Qt::WindingFill
                ? VectorPathFillRule::NonZero
                : VectorPathFillRule::EvenOdd;
        QCOMPARE(expanded.pathFillRule, expectedFillRule);
        QCOMPARE(expanded.fill.colour, source.stroke.colour);
        QCOMPARE(expanded.fill.opacity, source.stroke.opacity);
        QVERIFY(!expanded.stroke.enabled);
        QVERIFY(expanded.transform.isIdentity());
        QVERIFY(expanded.isSafe());
        if (source.stroke.pattern == VectorStrokePattern::Dashed) {
            QVERIFY2(!expanded.additionalBezierPaths.isEmpty(),
                     "Dashed expansion must preserve independent dash contours without synthetic connector spokes.");
            if (!source.isOpenPath()) {
                QVERIFY2(expanded.additionalBezierPaths.size() >= 4,
                         "A closed dashed outline must retain multiple independent dash islands.");
            }
        }
        if (!source.isOpenPath()
            && source.stroke.alignment == VectorStrokeAlignment::Centre) {
            QVERIFY2(!expanded.additionalBezierPaths.isEmpty(),
                     "Closed centre strokes must preserve the inner hole as a true compound contour.");
        }
        for (const VectorBezierPath &contour : expanded.additionalBezierPaths) {
            QVERIFY(contour.closed);
            QVERIFY(contour.isSafe());
        }

        const QPainterPath expected = source.strokePathForWorldTransform(layerWorld);
        const QPainterPath actual = expanded.pathForWorldTransform(layerWorld);
        QVERIFY2(coverageMatches(expected, actual),
                 qPrintable(vectorShapeTypeDisplayName(source.type)));

        LayerNode originalLayer;
        originalLayer.type = LayerType::Vector;
        originalLayer.transform = layerWorld;
        VectorShape renderSource = source;
        renderSource.fill.enabled = false;
        originalLayer.vectorData.objects = {renderSource};
        originalLayer.vectorData.normalise();

        LayerNode expandedLayer;
        expandedLayer.type = LayerType::Vector;
        expandedLayer.transform = layerWorld;
        expandedLayer.vectorData.objects = {expanded};
        expandedLayer.vectorData.normalise();

        const QImage originalImage = VectorRasterizer::renderLayerRegion(
            originalLayer, documentSize, region, documentSize, QTransform(),
            QImage::Format_RGBA8888, colourSpace);
        const QImage expandedImage = VectorRasterizer::renderLayerRegion(
            expandedLayer, documentSize, region, documentSize, QTransform(),
            QImage::Format_RGBA8888, colourSpace);
        QVERIFY(!originalImage.isNull());
        QVERIFY(!expandedImage.isNull());
        QVERIFY2(imagesWithinChannelTolerance(originalImage, expandedImage, 3),
                 qPrintable(vectorShapeTypeDisplayName(source.type)));

        bool jsonOk = false;
        const QJsonObject json = expanded.toJson(&jsonOk);
        QVERIFY(jsonOk);
        const VectorShape decoded = VectorShape::fromJson(json, &jsonOk);
        QVERIFY(jsonOk);
        QCOMPARE(decoded, expanded);

        VectorLayerData compoundData;
        compoundData.objects = {expanded};
        compoundData.normalise();
        const QJsonObject compoundJson = compoundData.toJson(&jsonOk);
        QVERIFY(jsonOk);
        if (!expanded.additionalBezierPaths.isEmpty()) {
            QJsonObject dishonest = compoundJson;
            dishonest.insert(QStringLiteral("schema"), 4);
            bool dishonestOk = false;
            VectorLayerData::fromJson(dishonest, &dishonestOk);
            QVERIFY(!dishonestOk);
        }
        {
            QJsonObject dishonest = compoundJson;
            dishonest.insert(QStringLiteral("schema"), 5);
            bool dishonestOk = false;
            VectorLayerData::fromJson(dishonest, &dishonestOk);
            QVERIFY2(!dishonestOk,
                     "Schema 5 must not claim nonzero path-fill metadata.");
        }
    }

    // Closed dashed paths can make Qt's stroker emit zero-area seam debris
    // alongside the real dash islands. Those fragments must be ignored rather
    // than causing the complete Expand Stroke operation to fail. Exercise dash
    // phases on both sides of the dash/gap and period boundaries.
    const QVector<double> seamOffsets {
        0.0, 1.0e-9, 20.999999, 21.0, 32.999999, 33.0, -1.0e-9, -33.0
    };
    for (const double offset : seamOffsets) {
        VectorShape seamCase = dashedRectangle;
        seamCase.stroke.dashOffset = offset;
        seamCase.normalise();
        VectorShape expanded;
        QVERIFY2(seamCase.expandedStrokePath(layerWorld, &expanded),
                 qPrintable(QStringLiteral("closed dashed seam offset %1")
                                .arg(offset, 0, 'g', 12)));
        QVERIFY(expanded.bezierPath.closed);
        QVERIFY(!expanded.additionalBezierPaths.isEmpty());
        QVERIFY(expanded.isSafe());
        const QPainterPath expected = seamCase.strokePathForWorldTransform(
            layerWorld);
        const QPainterPath actual = expanded.pathForWorldTransform(layerWorld);
        QVERIFY2(coverageMatches(expected, actual),
                 qPrintable(QStringLiteral("closed dashed coverage offset %1")
                                .arg(offset, 0, 'g', 12)));
    }

    // A filled, stroked object must keep layer-level compositing exactly
    // once when the command promotes it to an isolated group of render
    // components. This mirrors the MainWindow structural commit while
    // keeping the geometry conversion itself model-level.
    VectorShape filledAndStroked;
    filledAndStroked.type = VectorShapeType::RoundedRectangle;
    filledAndStroked.bounds = QRectF(54.0, 42.0, 154.0, 108.0);
    filledAndStroked.cornerRadii.topLeft = 18.0;
    filledAndStroked.cornerRadii.topRight = 31.0;
    filledAndStroked.cornerRadii.bottomRight = 12.0;
    filledAndStroked.cornerRadii.bottomLeft = 24.0;
    filledAndStroked.fill.enabled = true;
    filledAndStroked.fill.colour = QColor(64, 178, 206, 181);
    filledAndStroked.fill.opacity = 0.58;
    filledAndStroked.stroke.enabled = true;
    filledAndStroked.stroke.colour = QColor(236, 91, 44, 214);
    filledAndStroked.stroke.opacity = 0.76;
    filledAndStroked.stroke.width = 15.0;
    filledAndStroked.stroke.alignment = VectorStrokeAlignment::Centre;
    filledAndStroked.stroke.join = VectorStrokeJoin::Round;
    filledAndStroked.transform.rotate(7.0);
    filledAndStroked.normalise();

    LayerNode originalComposite;
    originalComposite.type = LayerType::Vector;
    originalComposite.name = QStringLiteral("Filled Stroke");
    originalComposite.opacity = 0.71;
    originalComposite.blendMode = BlendMode::Multiply;
    originalComposite.transform.translate(13.0, 9.0);
    originalComposite.transform.shear(0.08, -0.04);
    originalComposite.vectorData.objects = {filledAndStroked};
    originalComposite.vectorData.normalise();
    originalComposite.maskImage = QImage(documentSize, QImage::Format_Grayscale8);
    originalComposite.maskImage.fill(255);
    {
        QPainter maskPainter(&originalComposite.maskImage);
        maskPainter.fillRect(QRect(0, 0, documentSize.width() / 2,
                                   documentSize.height()), QColor(128, 128, 128));
    }
    originalComposite.maskReferenceSize = documentSize;
    originalComposite.maskEnabled = true;

    VectorShape expandedCompositeStroke;
    QVERIFY(filledAndStroked.expandedStrokePath(
        originalComposite.transform, &expandedCompositeStroke));
    VectorShape retainedFill = filledAndStroked;
    retainedFill.stroke.enabled = false;
    retainedFill.normalise();

    LayerNode fillChild;
    fillChild.type = LayerType::Vector;
    fillChild.name = QStringLiteral("Fill");
    fillChild.blendMode = BlendMode::Copy;
    fillChild.vectorData.objects = {retainedFill};
    fillChild.vectorData.normalise();

    LayerNode strokeChild;
    strokeChild.type = LayerType::Vector;
    strokeChild.name = QStringLiteral("Expanded Stroke");
    strokeChild.blendMode = BlendMode::Copy;
    strokeChild.vectorData.objects = {expandedCompositeStroke};
    strokeChild.vectorData.normalise();

    LayerNode expandedComposite = originalComposite;
    expandedComposite.type = LayerType::Group;
    expandedComposite.groupCompositeMode = GroupCompositeMode::Isolated;
    expandedComposite.vectorData = {};
    // Child stacks are stored top-first and composited in reverse. The stroke
    // therefore appears above the fill, matching one VectorRasterizer pass.
    expandedComposite.children = {strokeChild, fillChild};

    QImage compositeBase(documentSize, QImage::Format_RGBA8888);
    compositeBase.fill(QColor(153, 127, 91, 255));
    compositeBase.setColorSpace(colourSpace);
    LayerNode baseLayer;
    baseLayer.type = LayerType::BaseImage;
    const QImage originalCompositeImage = ImageProcessor::render(
        compositeBase, {originalComposite, baseLayer}, nullptr, documentSize);
    const QImage expandedCompositeImage = ImageProcessor::render(
        compositeBase, {expandedComposite, baseLayer}, nullptr, documentSize);
    QVERIFY(!originalCompositeImage.isNull());
    QVERIFY(!expandedCompositeImage.isNull());
    QVERIFY(imagesWithinChannelTolerance(originalCompositeImage,
                                          expandedCompositeImage, 3));

    // A non-zero layer Feather cannot be split across the fill/stroke child
    // layers above because that would feather each component independently.
    // The 15d command path instead keeps the expanded components in one vector
    // layer, preserving the same combined-silhouette Feather boundary.
    LayerNode featheredOriginal = originalComposite;
    featheredOriginal.vectorData.featherRadius = 6.0;
    featheredOriginal.vectorData.normalise();
    LayerNode featheredExpanded = originalComposite;
    featheredExpanded.vectorData.objects = {retainedFill, expandedCompositeStroke};
    featheredExpanded.vectorData.featherRadius = 6.0;
    featheredExpanded.vectorData.normalise();
    QVERIFY(featheredOriginal.vectorData.isSafe());
    QVERIFY(featheredExpanded.vectorData.isSafe());
    const QImage featheredOriginalImage = ImageProcessor::render(
        compositeBase, {featheredOriginal, baseLayer}, nullptr, documentSize);
    const QImage featheredExpandedImage = ImageProcessor::render(
        compositeBase, {featheredExpanded, baseLayer}, nullptr, documentSize);
    QVERIFY(!featheredOriginalImage.isNull());
    QVERIFY(!featheredExpandedImage.isNull());
    QVERIFY2(imagesWithinChannelTolerance(featheredOriginalImage,
                                           featheredExpandedImage, 3),
             "Feather-preserving Expand Stroke changed the combined vector silhouette.");

    VectorShape disabled = openDashed;
    disabled.stroke.enabled = false;
    disabled.fill.enabled = true;
    disabled.type = VectorShapeType::Rectangle;
    disabled.bounds = QRectF(0.0, 0.0, 20.0, 20.0);
    disabled.normalise();
    VectorShape ignored;
    QVERIFY(!disabled.expandedStrokePath(QTransform(), &ignored));
    QTransform singular;
    singular.scale(0.0, 1.0);
    QVERIFY(!openDashed.expandedStrokePath(singular, &ignored));

    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Expanded Stroke Project");
    settings.pixelSize = QSize(256, 192);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QUuid layerId = document.addVectorShape(
        VectorShapeType::Ellipse, QRectF(34.0, 29.0, 120.0, 82.0),
        QColor(22, 44, 66, 180));
    QVERIFY(!layerId.isNull());
    LayerNode layer = document.layerById(layerId);
    VectorShape &projectSource = layer.vectorData.objects.first();
    projectSource.fill.enabled = false;
    projectSource.stroke.enabled = true;
    projectSource.stroke.colour = QColor(210, 90, 35, 201);
    projectSource.stroke.opacity = 0.77;
    projectSource.stroke.width = 14.0;
    projectSource.stroke.alignment = VectorStrokeAlignment::Outside;
    projectSource.stroke.pattern = VectorStrokePattern::Dashed;
    projectSource.stroke.dashLength = 18.0;
    projectSource.stroke.gapLength = 7.0;
    projectSource.normalise();
    VectorShape projectExpanded;
    QVERIFY(projectSource.expandedStrokePath(
        document.layerWorldTransform(layerId), &projectExpanded));
    projectExpanded.id = projectSource.id;
    projectExpanded.normalise();
    layer.vectorData.objects = {projectExpanded};
    layer.vectorData.normalise();
    QVERIFY(document.replaceLayer(layerId, layer));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("expanded-stroke.vfxphoto"));
    QVERIFY2(document.saveProject(path, &error), qPrintable(error));
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(path, &error), qPrintable(error));
    QCOMPARE(restored.layerById(layerId), document.layerById(layerId));

    QFile projectFile(path);
    QVERIFY(projectFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    QJsonDocument downgradedDocument = QJsonDocument::fromJson(
        projectFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    projectFile.close();
    QJsonObject downgradedRoot = downgradedDocument.object();
    downgradedRoot.insert(QStringLiteral("version"), 12);
    const QString downgradedPath = directory.filePath(
        QStringLiteral("expanded-stroke-pre-winding.vfxphoto"));
    QFile downgradedFile(downgradedPath);
    QVERIFY(downgradedFile.open(QIODevice::WriteOnly));
    const QByteArray downgradedBytes = QJsonDocument(downgradedRoot).toJson();
    QCOMPARE(downgradedFile.write(downgradedBytes),
             static_cast<qint64>(downgradedBytes.size()));
    downgradedFile.close();
    PhotoDocument rejected;
    error.clear();
    QVERIFY(!rejected.loadProject(downgradedPath, &error));
    QVERIFY(error.contains(QStringLiteral("version-13"), Qt::CaseInsensitive));
}


void CoreTests::vectorAppearanceRoundTripsSwapsAndApplies()
{
    VectorShape source;
    source.type = VectorShapeType::RoundedRectangle;
    source.bounds = QRectF(12.0, 18.0, 84.0, 56.0);
    source.fill.enabled = true;
    source.fill.colour = QColor(24, 96, 188, 173);
    source.fill.opacity = 0.42;
    source.stroke.enabled = true;
    source.stroke.colour = QColor(231, 76, 44, 209);
    source.stroke.opacity = 0.77;
    source.stroke.width = 9.25;
    source.stroke.alignment = VectorStrokeAlignment::Outside;
    source.stroke.cap = VectorStrokeCap::Square;
    source.stroke.join = VectorStrokeJoin::Bevel;
    source.stroke.miterLimit = 12.5;
    source.stroke.pattern = VectorStrokePattern::Dashed;
    source.stroke.dashLength = 17.0;
    source.stroke.gapLength = 6.0;
    source.stroke.dashOffset = -3.5;
    source.stroke.startArrowhead = VectorArrowheadType::Open;
    source.stroke.endArrowhead = VectorArrowheadType::Stealth;
    source.stroke.startArrowScale = 1.25;
    source.stroke.endArrowScale = 1.8;
    source.normalise();
    QVERIFY(source.isSafe());

    const VectorAppearance appearance = VectorAppearance::fromShape(source);
    QVERIFY(appearance.isSafe());
    QCOMPARE(appearance.fill, source.fill);
    QCOMPARE(appearance.stroke, source.stroke);

    bool encodedOk = false;
    const QJsonObject encoded = appearance.toJson(&encodedOk);
    QVERIFY(encodedOk);
    QCOMPARE(encoded.value(QStringLiteral("format")).toString(),
             QStringLiteral("VFXPhotoLabVectorAppearance"));
    QCOMPARE(encoded.value(QStringLiteral("schema")).toInt(), 2);
    bool decodedOk = false;
    const VectorAppearance decoded = VectorAppearance::fromJson(encoded,
                                                                 &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(decoded, appearance);

    VectorAppearance swapped = appearance;
    swapped.swapFillAndStroke();
    QVERIFY(swapped.isSafe());
    QCOMPARE(swapped.fill.enabled, source.stroke.enabled);
    QCOMPARE(swapped.fill.colour, source.stroke.colour);
    QCOMPARE(swapped.fill.opacity, source.stroke.opacity);
    QCOMPARE(swapped.stroke.enabled, source.fill.enabled);
    QCOMPARE(swapped.stroke.colour, source.fill.colour);
    QCOMPARE(swapped.stroke.opacity, source.fill.opacity);
    QCOMPARE(swapped.stroke.width, source.stroke.width);
    QCOMPARE(swapped.stroke.pattern, source.stroke.pattern);
    QCOMPARE(swapped.stroke.dashLength, source.stroke.dashLength);
    QCOMPARE(swapped.stroke.gapLength, source.stroke.gapLength);
    QCOMPARE(swapped.stroke.dashOffset, source.stroke.dashOffset);
    QCOMPARE(swapped.stroke.cap, source.stroke.cap);
    QCOMPARE(swapped.stroke.join, source.stroke.join);
    QCOMPARE(swapped.stroke.alignment, source.stroke.alignment);
    QCOMPARE(swapped.stroke.miterLimit, source.stroke.miterLimit);
    QCOMPARE(swapped.stroke.startArrowhead, source.stroke.startArrowhead);
    QCOMPARE(swapped.stroke.endArrowhead, source.stroke.endArrowhead);
    QCOMPARE(swapped.stroke.startArrowScale, source.stroke.startArrowScale);
    QCOMPARE(swapped.stroke.endArrowScale, source.stroke.endArrowScale);

    QJsonObject legacyAppearance = encoded;
    legacyAppearance.insert(QStringLiteral("schema"), 1);
    QJsonObject legacyStroke = legacyAppearance.value(QStringLiteral("stroke")).toObject();
    legacyStroke.remove(QStringLiteral("startArrowhead"));
    legacyStroke.remove(QStringLiteral("endArrowhead"));
    legacyStroke.remove(QStringLiteral("startArrowScale"));
    legacyStroke.remove(QStringLiteral("endArrowScale"));
    legacyAppearance.insert(QStringLiteral("stroke"), legacyStroke);
    const VectorAppearance migratedLegacy = VectorAppearance::fromJson(
        legacyAppearance, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(migratedLegacy.schema, VectorAppearance::CurrentSchema);
    QCOMPARE(migratedLegacy.stroke.startArrowhead, VectorArrowheadType::None);
    QCOMPARE(migratedLegacy.stroke.endArrowhead, VectorArrowheadType::None);

    VectorShape target;
    target.type = VectorShapeType::Star;
    target.bounds = QRectF(-20.0, 7.0, 130.0, 90.0);
    target.polygonSides = 9;
    target.starInnerRatio = 0.31;
    target.transform.translate(8.0, 13.0);
    target.normalise();
    const QUuid targetId = target.id;
    const QRectF targetBounds = target.bounds;
    const QTransform targetTransform = target.transform;
    const int targetSides = target.polygonSides;
    decoded.applyTo(target);
    QVERIFY(target.isSafe());
    QCOMPARE(target.id, targetId);
    QCOMPARE(target.type, VectorShapeType::Star);
    QCOMPARE(target.bounds, targetBounds);
    QCOMPARE(target.transform, targetTransform);
    QCOMPARE(target.polygonSides, targetSides);
    QCOMPARE(target.fill, source.fill);
    QCOMPARE(target.stroke, source.stroke);

    VectorShape line;
    line.type = VectorShapeType::Line;
    line.lineStart = QPointF(1.0, 2.0);
    line.lineEnd = QPointF(30.0, 12.0);
    line.normalise();
    VectorAppearance fillOnly = appearance;
    fillOnly.stroke.enabled = false;
    fillOnly.stroke.alignment = VectorStrokeAlignment::Outside;
    fillOnly.applyTo(line);
    QVERIFY(line.isSafe());
    QVERIFY(!line.fill.enabled);
    QVERIFY(line.stroke.enabled);
    QCOMPARE(line.stroke.alignment, VectorStrokeAlignment::Centre);
    QCOMPARE(line.stroke.colour, fillOnly.stroke.colour);
    QCOMPARE(line.stroke.width, fillOnly.stroke.width);

    VectorShape openPath;
    openPath.type = VectorShapeType::Path;
    VectorPathNode first;
    first.anchor = QPointF(0.0, 0.0);
    VectorPathNode second;
    second.anchor = QPointF(20.0, 10.0);
    openPath.bezierPath.nodes = {first, second};
    openPath.fill.enabled = false;
    openPath.stroke.enabled = true;
    openPath.normalise();
    VectorAppearance openSwapped = VectorAppearance::fromShape(openPath);
    openSwapped.swapFillAndStroke();
    openSwapped.applyTo(openPath);
    QVERIFY(openPath.isSafe());
    QVERIFY(openPath.stroke.enabled);

    const QColor primary(11, 22, 33, 244);
    const QColor secondary(210, 190, 170, 155);
    const VectorAppearance closedDefaults =
        VectorAppearance::sensibleDefaults(primary, secondary, false);
    QVERIFY(closedDefaults.fill.enabled);
    QVERIFY(!closedDefaults.stroke.enabled);
    QCOMPARE(closedDefaults.fill.colour, secondary);
    QCOMPARE(closedDefaults.stroke.colour, primary);
    QCOMPARE(closedDefaults.stroke.width, 2.0);
    QCOMPARE(closedDefaults.stroke.cap, VectorStrokeCap::Round);
    QCOMPARE(closedDefaults.stroke.join, VectorStrokeJoin::Miter);
    QCOMPARE(closedDefaults.stroke.pattern, VectorStrokePattern::Solid);

    const VectorAppearance openDefaults =
        VectorAppearance::sensibleDefaults(primary, secondary, true);
    QVERIFY(!openDefaults.fill.enabled);
    QVERIFY(openDefaults.stroke.enabled);
    QCOMPARE(openDefaults.stroke.alignment, VectorStrokeAlignment::Centre);

    QJsonObject wrongSchema = encoded;
    wrongSchema.insert(QStringLiteral("schema"), 3);
    VectorAppearance::fromJson(wrongSchema, &decodedOk);
    QVERIFY(!decodedOk);
    QJsonObject malformedArrowScale = encoded;
    QJsonObject malformedArrowStroke = malformedArrowScale.value(
        QStringLiteral("stroke")).toObject();
    malformedArrowStroke.insert(QStringLiteral("endArrowScale"),
                                QStringLiteral("huge"));
    malformedArrowScale.insert(QStringLiteral("stroke"), malformedArrowStroke);
    VectorAppearance::fromJson(malformedArrowScale, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject missingStroke = encoded;
    missingStroke.remove(QStringLiteral("stroke"));
    VectorAppearance::fromJson(missingStroke, &decodedOk);
    QVERIFY(!decodedOk);
    QJsonObject wrongFormat = encoded;
    wrongFormat.insert(QStringLiteral("format"), QStringLiteral("Other"));
    VectorAppearance::fromJson(wrongFormat, &decodedOk);
    QVERIFY(!decodedOk);
}

void CoreTests::vectorAppearancePresetStoreSavesRenamesAndDeletes()
{
    QStandardPaths::setTestModeEnabled(true);

    VectorAppearance appearance;
    appearance.fill.enabled = true;
    appearance.fill.colour = QColor(31, 82, 190, 171);
    appearance.fill.opacity = 0.64;
    appearance.stroke.enabled = true;
    appearance.stroke.colour = QColor(229, 114, 42, 207);
    appearance.stroke.opacity = 0.81;
    appearance.stroke.width = 7.5;
    appearance.stroke.alignment = VectorStrokeAlignment::Outside;
    appearance.stroke.cap = VectorStrokeCap::Square;
    appearance.stroke.join = VectorStrokeJoin::Bevel;
    appearance.stroke.miterLimit = 9.0;
    appearance.stroke.pattern = VectorStrokePattern::Dashed;
    appearance.stroke.dashLength = 15.0;
    appearance.stroke.gapLength = 5.0;
    appearance.stroke.dashOffset = -2.0;
    appearance.stroke.startArrowhead = VectorArrowheadType::Diamond;
    appearance.stroke.endArrowhead = VectorArrowheadType::Stealth;
    appearance.stroke.startArrowScale = 1.35;
    appearance.stroke.endArrowScale = 1.75;
    appearance.normalise();
    QVERIFY(appearance.isSafe());

    const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString originalName = QStringLiteral("Vector Style %1").arg(suffix);
    const QString renamedName = QStringLiteral("Renamed Style %1").arg(suffix);
    QString error;
    QVERIFY2(VectorAppearancePresetStore::saveUserPreset(
                 originalName, appearance, &error), qPrintable(error));

    QVector<VectorAppearancePreset> presets =
        VectorAppearancePresetStore::presets();
    auto found = std::find_if(
        presets.cbegin(), presets.cend(), [&](const auto &preset) {
            return preset.name == originalName;
        });
    QVERIFY(found != presets.cend());
    QCOMPARE(found->appearance, appearance);
    QVERIFY(QFileInfo::exists(found->storagePath));

    QFile file(found->storagePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument stored = QJsonDocument::fromJson(
        file.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(stored.isObject());
    QCOMPARE(stored.object().value(QStringLiteral("format")).toString(),
             QStringLiteral("vfxphotolab-preset"));
    QCOMPARE(stored.object().value(QStringLiteral("version")).toInt(),
             PresetStore::CurrentSchemaVersion);
    QCOMPARE(stored.object().value(QStringLiteral("kind")).toString(),
             QStringLiteral("vector-appearance"));
    QCOMPARE(stored.object().value(QStringLiteral("metadata")).toObject()
                 .value(QStringLiteral("category")).toString(),
             QStringLiteral("Vector Appearance"));

    VectorAppearance overwritten = appearance;
    overwritten.stroke.width = 13.25;
    overwritten.stroke.endArrowhead = VectorArrowheadType::Circle;
    overwritten.normalise();
    QVERIFY2(VectorAppearancePresetStore::saveUserPreset(
                 originalName, overwritten, &error), qPrintable(error));
    presets = VectorAppearancePresetStore::presets();
    found = std::find_if(
        presets.cbegin(), presets.cend(), [&](const auto &preset) {
            return preset.name == originalName;
        });
    QVERIFY(found != presets.cend());
    QCOMPARE(found->appearance, overwritten);
    const VectorAppearancePreset saved = *found;

    QVERIFY2(VectorAppearancePresetStore::renameUserPreset(
                 saved, renamedName, &error), qPrintable(error));
    presets = VectorAppearancePresetStore::presets();
    QVERIFY(std::none_of(
        presets.cbegin(), presets.cend(), [&](const auto &preset) {
            return preset.name == originalName;
        }));
    const auto renamed = std::find_if(
        presets.cbegin(), presets.cend(), [&](const auto &preset) {
            return preset.name == renamedName;
        });
    QVERIFY(renamed != presets.cend());
    QCOMPARE(renamed->appearance, overwritten);

    const VectorAppearancePreset renamedPreset = *renamed;
    QVERIFY2(VectorAppearancePresetStore::removeUserPreset(
                 renamedPreset, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(renamedPreset.storagePath));
}

void CoreTests::parameterisedVectorShapesRoundTripAndRejectMalformedPayloads()
{
    VectorShape line;
    line.type = VectorShapeType::Line;
    line.lineStart = QPointF(42.5, 9.25);
    line.lineEnd = QPointF(-7.75, 31.5);
    line.fill.enabled = false;
    line.stroke.enabled = true;
    line.stroke.colour = QColor::fromRgba64(
        QRgba64::fromRgba64(1000, 22000, 51000, 43000));
    line.stroke.opacity = 0.73;
    line.stroke.width = 6.5;
    line.stroke.alignment = VectorStrokeAlignment::Centre;
    line.stroke.cap = VectorStrokeCap::Square;
    line.stroke.join = VectorStrokeJoin::Bevel;
    line.stroke.miterLimit = 7.0;
    line.normalise();
    QVERIFY(line.isSafe());
    QVERIFY(!line.fill.enabled);
    QVERIFY(line.stroke.enabled);
    QCOMPARE(line.bounds, QRectF(line.lineStart, line.lineEnd).normalized());

    VectorShape polygon;
    polygon.type = VectorShapeType::Polygon;
    polygon.bounds = QRectF(-12.0, 4.0, 80.0, 56.0);
    polygon.polygonSides = 7;
    polygon.vertexRotationDegrees = 18.0;
    polygon.fill.enabled = true;
    polygon.fill.colour = QColor(20, 180, 90, 210);
    polygon.stroke.enabled = true;
    polygon.stroke.width = 3.25;
    polygon.stroke.alignment = VectorStrokeAlignment::Inside;
    polygon.stroke.cap = VectorStrokeCap::Round;
    polygon.stroke.join = VectorStrokeJoin::Round;
    polygon.normalise();
    QVERIFY(polygon.isSafe());

    VectorShape star;
    star.type = VectorShapeType::Star;
    star.bounds = QRectF(100.0, -30.0, 72.0, 72.0);
    star.polygonSides = 9;
    star.starInnerRatio = 0.37;
    star.vertexRotationDegrees = -126.5;
    star.fill.enabled = false;
    star.stroke.enabled = true;
    star.stroke.width = 11.0;
    star.stroke.alignment = VectorStrokeAlignment::Outside;
    star.stroke.cap = VectorStrokeCap::Butt;
    star.stroke.join = VectorStrokeJoin::Miter;
    star.stroke.miterLimit = 8.0;
    star.normalise();
    QVERIFY(star.isSafe());

    VectorLayerData source;
    source.objects = {line, polygon, star};
    source.normalise();
    QVERIFY(source.isSafe());
    bool encodedOk = false;
    const QJsonObject encoded = source.toJson(&encodedOk);
    QVERIFY(encodedOk);
    bool decodedOk = false;
    const VectorLayerData decoded = VectorLayerData::fromJson(encoded, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(decoded, source);
    QCOMPARE(decoded.objects.at(0).lineStart, line.lineStart);
    QCOMPARE(decoded.objects.at(0).lineEnd, line.lineEnd);
    QCOMPARE(decoded.objects.at(1).polygonSides, 7);
    QCOMPARE(decoded.objects.at(2).starInnerRatio, 0.37);
    QCOMPARE(decoded.objects.at(2).stroke.alignment,
             VectorStrokeAlignment::Outside);

    // 0.9.0a payloads did not include enabled fills or a stroke object. They
    // remain valid and migrate to an enabled fill with a disabled stroke.
    bool legacyShapeOk = false;
    QJsonObject legacyShape = polygon.toJson(&legacyShapeOk);
    QVERIFY(legacyShapeOk);
    QJsonObject legacyFill = legacyShape.value(QStringLiteral("fill")).toObject();
    legacyFill.remove(QStringLiteral("enabled"));
    legacyShape.insert(QStringLiteral("fill"), legacyFill);
    legacyShape.remove(QStringLiteral("stroke"));
    const VectorShape migrated = VectorShape::fromJson(legacyShape, &legacyShapeOk);
    QVERIFY(legacyShapeOk);
    QVERIFY(migrated.fill.enabled);
    QVERIFY(!migrated.stroke.enabled);

    QJsonObject malformedSides = polygon.toJson(&encodedOk);
    QVERIFY(encodedOk);
    malformedSides.insert(QStringLiteral("polygonSides"), 2);
    VectorShape malformed = VectorShape::fromJson(malformedSides, &decodedOk);
    QVERIFY(!decodedOk);
    QCOMPARE(malformed.polygonSides, 3);

    QJsonObject malformedRatio = star.toJson(&encodedOk);
    QVERIFY(encodedOk);
    malformedRatio.insert(QStringLiteral("starInnerRatio"), 1.5);
    VectorShape::fromJson(malformedRatio, &decodedOk);
    QVERIFY(!decodedOk);

    VectorShape semanticArrow;
    semanticArrow.type = VectorShapeType::Arrow;
    semanticArrow.bounds = QRectF(4.0, 8.0, 120.0, 54.0);
    semanticArrow.normalise();
    QJsonObject malformedArrow = semanticArrow.toJson(&encodedOk);
    QVERIFY(encodedOk);
    malformedArrow.insert(QStringLiteral("arrowHeadLengthRatio"),
                          QStringLiteral("invalid"));
    VectorShape::fromJson(malformedArrow, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject malformedStroke = line.toJson(&encodedOk);
    QVERIFY(encodedOk);
    QJsonObject strokeObject = malformedStroke.value(QStringLiteral("stroke")).toObject();
    strokeObject.insert(QStringLiteral("width"), 0.0);
    malformedStroke.insert(QStringLiteral("stroke"), strokeObject);
    VectorShape::fromJson(malformedStroke, &decodedOk);
    QVERIFY(!decodedOk);

    QJsonObject illegalLineAlignment = line.toJson(&encodedOk);
    QVERIFY(encodedOk);
    strokeObject = illegalLineAlignment.value(QStringLiteral("stroke")).toObject();
    strokeObject.insert(QStringLiteral("alignment"), QStringLiteral("outside"));
    illegalLineAlignment.insert(QStringLiteral("stroke"), strokeObject);
    VectorShape::fromJson(illegalLineAlignment, &decodedOk);
    QVERIFY(!decodedOk);
}


void CoreTests::vectorArrowheadsAndArrowShapeRoundTripExpandAndPersist()
{
    VectorShape centredMarkerLine;
    centredMarkerLine.type = VectorShapeType::Line;
    centredMarkerLine.lineStart = QPointF(100.0, 100.0);
    centredMarkerLine.lineEnd = QPointF(200.0, 100.0);
    centredMarkerLine.fill.enabled = false;
    centredMarkerLine.stroke.enabled = true;
    centredMarkerLine.stroke.width = 10.0;
    centredMarkerLine.stroke.cap = VectorStrokeCap::Round;
    centredMarkerLine.stroke.startArrowhead = VectorArrowheadType::Circle;
    centredMarkerLine.stroke.endArrowhead = VectorArrowheadType::Triangle;
    centredMarkerLine.normalise();
    const QRectF centredMarkerBounds = centredMarkerLine
        .strokePathForWorldTransform(QTransform()).boundingRect();
    // Circle markers are centred on the start endpoint, while a 1x triangle
    // extends half of its 4*stroke-width length beyond the end endpoint. The
    // shaft's round cap therefore remains inside the marker instead of
    // protruding through its point.
    QVERIFY(std::abs(centredMarkerBounds.left() - 85.0) <= 0.01);
    QVERIFY(std::abs(centredMarkerBounds.right() - 220.0) <= 0.01);

    centredMarkerLine.stroke.startArrowhead = VectorArrowheadType::None;
    centredMarkerLine.stroke.endArrowScale = 0.1;
    centredMarkerLine.normalise();
    const QRectF tinyMarkerBounds = centredMarkerLine
        .strokePathForWorldTransform(QTransform()).boundingRect();
    // Even at the minimum marker scale, the 5 px round cap is clipped at the
    // endpoint and cannot overtake the triangle's point at x=202.
    QVERIFY(std::abs(tinyMarkerBounds.right() - 202.0) <= 0.01);

    // Arrowhead cap clipping must remain local to the endpoint. The final
    // segment points to the right from x=0 to x=60, while unrelated earlier
    // segments loop far into x>60. The old endpoint half-plane clip removed
    // those distant segments whenever an end arrow was present.
    VectorShape windingPath;
    windingPath.type = VectorShapeType::Path;
    for (const QPointF &anchor : {
             QPointF(0.0, 0.0), QPointF(100.0, 0.0),
             QPointF(100.0, 100.0), QPointF(0.0, 100.0),
             QPointF(0.0, 20.0), QPointF(60.0, 20.0)}) {
        VectorPathNode node;
        node.anchor = anchor;
        windingPath.bezierPath.nodes.push_back(node);
    }
    windingPath.bezierPath.closed = false;
    windingPath.fill.enabled = false;
    windingPath.stroke.enabled = true;
    windingPath.stroke.width = 10.0;
    windingPath.stroke.endArrowhead = VectorArrowheadType::Triangle;
    windingPath.normalise();
    QVERIFY(windingPath.isSafe());
    const QPointF retainedFarSegment(100.0, 50.0);
    for (const VectorStrokeCap cap : {VectorStrokeCap::Butt,
                                      VectorStrokeCap::Round,
                                      VectorStrokeCap::Square}) {
        windingPath.stroke.cap = cap;
        windingPath.normalise();
        const QPainterPath withEndArrow = windingPath
            .strokePathForWorldTransform(QTransform());
        QVERIFY2(withEndArrow.contains(retainedFarSegment),
                 "An end arrow clipped an unrelated winding-path segment");

        VectorShape startArrowPath = windingPath;
        std::reverse(startArrowPath.bezierPath.nodes.begin(),
                     startArrowPath.bezierPath.nodes.end());
        startArrowPath.stroke.startArrowhead = VectorArrowheadType::Triangle;
        startArrowPath.stroke.endArrowhead = VectorArrowheadType::None;
        startArrowPath.normalise();
        const QPainterPath withStartArrow = startArrowPath
            .strokePathForWorldTransform(QTransform());
        QVERIFY2(withStartArrow.contains(retainedFarSegment),
                 "A start arrow clipped an unrelated winding-path segment");
    }
    windingPath.stroke.cap = VectorStrokeCap::Round;
    windingPath.normalise();
    VectorShape expandedWindingPath;
    QVERIFY(windingPath.expandedStrokePath(QTransform(), &expandedWindingPath));
    QVERIFY2(expandedWindingPath.geometryPath().contains(retainedFarSegment),
             "Expand Stroke lost an unrelated winding-path segment");

    VectorShape line;
    line.type = VectorShapeType::Line;
    line.lineStart = QPointF(42.0, 88.0);
    line.lineEnd = QPointF(238.0, 54.0);
    line.fill.enabled = false;
    line.stroke.enabled = true;
    line.stroke.colour = QColor(38, 77, 210, 224);
    line.stroke.opacity = 0.81;
    line.stroke.width = 9.0;
    line.stroke.cap = VectorStrokeCap::Round;
    line.stroke.startArrowhead = VectorArrowheadType::Open;
    line.stroke.endArrowhead = VectorArrowheadType::Stealth;
    line.stroke.startArrowScale = 1.2;
    line.stroke.endArrowScale = 1.65;
    line.normalise();
    QVERIFY(line.isSafe());
    QVERIFY(line.isOpenPath());

    VectorShape shaft = line;
    shaft.stroke.startArrowhead = VectorArrowheadType::None;
    shaft.stroke.endArrowhead = VectorArrowheadType::None;
    shaft.normalise();
    const QPainterPath shaftCoverage = shaft.strokePathForWorldTransform(QTransform());
    const QPainterPath arrowCoverage = line.strokePathForWorldTransform(QTransform());
    QVERIFY(!shaftCoverage.isEmpty());
    QVERIFY(!arrowCoverage.isEmpty());
    QVERIFY(arrowCoverage.boundingRect().width() > shaftCoverage.boundingRect().width());
    QVERIFY(arrowCoverage.elementCount() > shaftCoverage.elementCount());

    VectorShape expanded;
    QVERIFY(line.expandedStrokePath(QTransform(), &expanded));
    QCOMPARE(expanded.type, VectorShapeType::Path);
    QVERIFY(expanded.fill.enabled);
    QVERIFY(!expanded.stroke.enabled);
    QVERIFY(expanded.bezierPath.closed);
    const QRectF expectedBounds = arrowCoverage.boundingRect();
    const QRectF actualBounds = expanded.geometryPath().boundingRect();
    QVERIFY(std::abs(expectedBounds.left() - actualBounds.left()) <= 0.05);
    QVERIFY(std::abs(expectedBounds.top() - actualBounds.top()) <= 0.05);
    QVERIFY(std::abs(expectedBounds.right() - actualBounds.right()) <= 0.05);
    QVERIFY(std::abs(expectedBounds.bottom() - actualBounds.bottom()) <= 0.05);

    bool jsonOk = false;
    const QJsonObject lineJson = line.toJson(&jsonOk);
    QVERIFY(jsonOk);
    const VectorShape decodedLine = VectorShape::fromJson(lineJson, &jsonOk);
    QVERIFY(jsonOk);
    QCOMPARE(decodedLine, line);

    VectorLayerData data;
    data.objects = {line};
    data.normalise();
    const QJsonObject dataJson = data.toJson(&jsonOk);
    QVERIFY(jsonOk);
    QCOMPARE(dataJson.value(QStringLiteral("schema")).toInt(), 8);
    QJsonObject dishonestData = dataJson;
    dishonestData.insert(QStringLiteral("schema"), 6);
    VectorLayerData::fromJson(dishonestData, &jsonOk);
    QVERIFY(!jsonOk);

    VectorShape blockArrow;
    blockArrow.type = VectorShapeType::Arrow;
    blockArrow.bounds = QRectF(18.0, 22.0, 176.0, 92.0);
    blockArrow.arrowHeadLengthRatio = 0.42;
    blockArrow.arrowShaftWidthRatio = 0.28;
    blockArrow.fill.enabled = true;
    blockArrow.fill.colour = QColor(230, 124, 38, 211);
    blockArrow.stroke.enabled = true;
    blockArrow.stroke.width = 4.0;
    blockArrow.normalise();
    QVERIFY(blockArrow.isSafe());
    QVERIFY(!blockArrow.isOpenPath());
    const QPainterPath semanticGeometry = blockArrow.geometryPath();
    QVERIFY(!semanticGeometry.isEmpty());
    QVERIFY(semanticGeometry.contains(blockArrow.bounds.center()));
    QVERIFY(semanticGeometry.contains(QPointF(blockArrow.bounds.right() - 1.0,
                                               blockArrow.bounds.center().y())));

    VectorShape convertedArrow = blockArrow;
    const QUuid arrowId = convertedArrow.id;
    QVERIFY(convertedArrow.convertToPath(QTransform()));
    QCOMPARE(convertedArrow.type, VectorShapeType::Path);
    QCOMPARE(convertedArrow.id, arrowId);
    QVERIFY(convertedArrow.bezierPath.closed);
    QVERIFY(convertedArrow.bezierPath.nodes.size() >= 7);
    QCOMPARE(convertedArrow.fill, blockArrow.fill);
    QCOMPARE(convertedArrow.stroke, blockArrow.stroke);
    const QRectF beforeBounds = semanticGeometry.boundingRect();
    const QRectF afterBounds = convertedArrow.geometryPath().boundingRect();
    QVERIFY(std::abs(beforeBounds.left() - afterBounds.left()) <= 1.0e-6);
    QVERIFY(std::abs(beforeBounds.right() - afterBounds.right()) <= 1.0e-6);
    QVERIFY(std::abs(beforeBounds.top() - afterBounds.top()) <= 1.0e-6);
    QVERIFY(std::abs(beforeBounds.bottom() - afterBounds.bottom()) <= 1.0e-6);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.name = QStringLiteral("Arrowheads");
    settings.pixelSize = QSize(320, 180);
    settings.bitDepth = 16;
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QUuid lineLayerId = document.addVectorShape(
        VectorShapeType::Line, QRectF(line.lineStart, line.lineEnd).normalized(),
        QColor(Qt::transparent), {}, 0.0, QLineF(line.lineStart, line.lineEnd));
    QVERIFY(!lineLayerId.isNull());
    QVERIFY(document.updateLayer(lineLayerId, [line](LayerNode &layer) {
        layer.vectorData.objects.first() = line;
        layer.vectorData.normalise();
    }));
    const QUuid arrowLayerId = document.addVectorShape(
        VectorShapeType::Arrow, blockArrow.bounds, blockArrow.fill.colour);
    QVERIFY(!arrowLayerId.isNull());
    QVERIFY(document.updateLayer(arrowLayerId, [blockArrow](LayerNode &layer) {
        layer.vectorData.objects.first() = blockArrow;
        layer.vectorData.normalise();
    }));

    const QString projectPath = directory.filePath(QStringLiteral("arrowheads.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    QFile projectFile(projectPath);
    QVERIFY(projectFile.open(QIODevice::ReadOnly));
    QJsonDocument projectJson = QJsonDocument::fromJson(projectFile.readAll());
    projectFile.close();
    QCOMPARE(projectJson.object().value(QStringLiteral("version")).toInt(), 15);
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QCOMPARE(restored.layerById(lineLayerId).vectorData.objects.constFirst(), line);
    QCOMPARE(restored.layerById(arrowLayerId).vectorData.objects.constFirst(), blockArrow);

    QJsonObject dishonestProject = projectJson.object();
    dishonestProject.insert(QStringLiteral("version"), 13);
    const QString dishonestPath = directory.filePath(QStringLiteral("arrowheads-v13.vfxphoto"));
    QFile dishonestFile(dishonestPath);
    QVERIFY(dishonestFile.open(QIODevice::WriteOnly));
    const QByteArray dishonestBytes = QJsonDocument(dishonestProject).toJson(
        QJsonDocument::Compact);
    QCOMPARE(dishonestFile.write(dishonestBytes),
             static_cast<qint64>(dishonestBytes.size()));
    dishonestFile.close();
    PhotoDocument rejected;
    error.clear();
    QVERIFY(!rejected.loadProject(dishonestPath, &error));
    QVERIFY(error.contains(QStringLiteral("version-14"), Qt::CaseInsensitive));
}

void CoreTests::vectorStrokesRespectAlignmentCapsAndBitDepth()
{
    VectorShape rectangle;
    rectangle.type = VectorShapeType::Rectangle;
    rectangle.bounds = QRectF(24.0, 20.0, 32.0, 28.0);
    rectangle.fill.enabled = false;
    rectangle.stroke.enabled = true;
    rectangle.stroke.width = 8.0;
    rectangle.stroke.colour = QColor::fromRgba64(
        QRgba64::fromRgba64(50000, 12000, 28000, 44000));
    rectangle.stroke.opacity = 0.75;
    rectangle.stroke.join = VectorStrokeJoin::Miter;
    rectangle.normalise();

    rectangle.stroke.alignment = VectorStrokeAlignment::Inside;
    const QRectF inside = rectangle.styledPathForWorldTransform({}).boundingRect();
    QVERIFY(std::abs(inside.left() - rectangle.bounds.left()) <= 1.0e-6);
    QVERIFY(std::abs(inside.top() - rectangle.bounds.top()) <= 1.0e-6);
    QVERIFY(std::abs(inside.right() - rectangle.bounds.right()) <= 1.0e-6);
    QVERIFY(std::abs(inside.bottom() - rectangle.bounds.bottom()) <= 1.0e-6);

    rectangle.stroke.alignment = VectorStrokeAlignment::Centre;
    const QRectF centre = rectangle.styledPathForWorldTransform({}).boundingRect();
    QVERIFY(std::abs(centre.left() - 20.0) <= 1.0e-6);
    QVERIFY(std::abs(centre.right() - 60.0) <= 1.0e-6);

    rectangle.stroke.alignment = VectorStrokeAlignment::Outside;
    const QRectF outside = rectangle.styledPathForWorldTransform({}).boundingRect();
    QVERIFY(std::abs(outside.left() - 16.0) <= 1.0e-6);
    QVERIFY(std::abs(outside.top() - 12.0) <= 1.0e-6);
    QVERIFY(std::abs(outside.right() - 64.0) <= 1.0e-6);
    QVERIFY(std::abs(outside.bottom() - 56.0) <= 1.0e-6);

    LayerNode rectangleLayer;
    rectangleLayer.type = LayerType::Vector;
    rectangleLayer.vectorData.objects = {rectangle};
    rectangleLayer.vectorData.normalise();
    QCOMPARE(VectorRasterizer::contentBounds(rectangleLayer), outside);

    const QSize size(96, 80);
    const QColorSpace colourSpace(QColorSpace::SRgb);
    const QImage rgba8 = VectorRasterizer::renderLayerRegion(
        rectangleLayer, size, QRect(QPoint(), size), size, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!rgba8.isNull());
    QVERIFY(rgba8.pixelColor(17, 30).alpha() > 0);
    QCOMPARE(rgba8.pixelColor(30, 30).alpha(), 0);

    const QImage rgba16 = VectorRasterizer::renderLayerRegion(
        rectangleLayer, size, QRect(QPoint(), size), size, QTransform(),
        QImage::Format_RGBA64, colourSpace);
    QVERIFY(!rgba16.isNull());
    const QRgba64 strokePixel = reinterpret_cast<const QRgba64 *>(
        rgba16.constScanLine(30))[17];
    const quint16 expectedAlpha = static_cast<quint16>(std::lround(
        rectangle.stroke.colour.alphaF() * rectangle.stroke.opacity * 65535.0));
    QVERIFY(std::abs(int(strokePixel.alpha()) - int(expectedAlpha)) <= 2);
    QVERIFY(std::abs(int(strokePixel.red())
                     - int(rectangle.stroke.colour.rgba64().red())) <= 2);

    VectorShape line;
    line.type = VectorShapeType::Line;
    line.lineStart = QPointF(30.0, 65.0);
    line.lineEnd = QPointF(60.0, 65.0);
    line.fill.enabled = false;
    line.stroke.enabled = true;
    line.stroke.width = 10.0;
    line.stroke.cap = VectorStrokeCap::Butt;
    line.normalise();
    const QRectF butt = line.styledPathForWorldTransform({}).boundingRect();
    line.stroke.cap = VectorStrokeCap::Round;
    const QRectF round = line.styledPathForWorldTransform({}).boundingRect();
    line.stroke.cap = VectorStrokeCap::Square;
    const QRectF square = line.styledPathForWorldTransform({}).boundingRect();
    QVERIFY(round.left() < butt.left() - 4.9);
    QVERIFY(round.right() > butt.right() + 4.9);
    QVERIFY(std::abs(round.left() - square.left()) <= 1.0e-6);
    QVERIFY(std::abs(round.right() - square.right()) <= 1.0e-6);

    VectorShape star;
    star.type = VectorShapeType::Star;
    star.bounds = QRectF(12.0, 10.0, 104.0, 104.0);
    star.polygonSides = 8;
    star.starInnerRatio = 0.42;
    star.vertexRotationDegrees = -90.0;
    star.fill.enabled = true;
    star.fill.colour = QColor(70, 185, 110, 255);
    star.stroke.enabled = true;
    star.stroke.colour = QColor(190, 75, 130, 255);
    star.stroke.width = 14.0;
    star.stroke.alignment = VectorStrokeAlignment::Inside;
    star.stroke.join = VectorStrokeJoin::Round;
    star.normalise();
    QVERIFY(star.isSafe());

    const QRectF semanticStarBounds = star.pathForWorldTransform({}).boundingRect();
    const QRectF styledStarBounds = star.styledPathForWorldTransform({}).boundingRect();
    QVERIFY(std::abs(styledStarBounds.left() - semanticStarBounds.left()) <= 1.0e-6);
    QVERIFY(std::abs(styledStarBounds.top() - semanticStarBounds.top()) <= 1.0e-6);
    QVERIFY(std::abs(styledStarBounds.right() - semanticStarBounds.right()) <= 1.0e-6);
    QVERIFY(std::abs(styledStarBounds.bottom() - semanticStarBounds.bottom()) <= 1.0e-6);

    LayerNode starLayer;
    starLayer.type = LayerType::Vector;
    starLayer.vectorData.objects = {star};
    starLayer.vectorData.normalise();
    const QSize starSize(128, 128);
    VectorRasterizer::clearCache();
    const QImage fullStar = VectorRasterizer::renderLayerRegion(
        starLayer, starSize, QRect(QPoint(), starSize), starSize, QTransform(),
        QImage::Format_RGBA8888, colourSpace);
    QVERIFY(!fullStar.isNull());

    QImage stitched(starSize, QImage::Format_RGBA8888);
    stitched.fill(Qt::transparent);
    for (int y = 0; y < starSize.height(); y += 64) {
        for (int x = 0; x < starSize.width(); x += 64) {
            const QRect region(x, y,
                               std::min(64, starSize.width() - x),
                               std::min(64, starSize.height() - y));
            const QImage tile = VectorRasterizer::renderLayerRegion(
                starLayer, starSize, region, starSize, QTransform(),
                QImage::Format_RGBA8888, colourSpace);
            QVERIFY(!tile.isNull());
            for (int row = 0; row < tile.height(); ++row) {
                std::memcpy(stitched.scanLine(region.y() + row) + region.x() * 4,
                            tile.constScanLine(row),
                            static_cast<size_t>(tile.width()) * 4);
            }
        }
    }
    QCOMPARE(stitched, fullStar);
}


void CoreTests::dashedVectorStrokesRoundTripScaleAndRejectPreVersionEleven()
{
    VectorShape line;
    line.type = VectorShapeType::Line;
    line.lineStart = QPointF(10.0, 24.0);
    line.lineEnd = QPointF(210.0, 24.0);
    line.fill.enabled = false;
    line.stroke.enabled = true;
    line.stroke.colour = QColor(25, 80, 220, 230);
    line.stroke.width = 6.0;
    line.stroke.cap = VectorStrokeCap::Round;
    line.stroke.pattern = VectorStrokePattern::Dashed;
    line.stroke.dashLength = 18.0;
    line.stroke.gapLength = 7.0;
    line.stroke.dashOffset = 3.0;
    line.normalise();
    QVERIFY(line.isSafe());

    const QPainterPath dashedOutline = line.strokeOutlineForWorldTransform({});
    QVERIFY(!dashedOutline.isEmpty());
    VectorShape solid = line;
    solid.stroke.pattern = VectorStrokePattern::Solid;
    solid.normalise();
    const QPainterPath solidOutline = solid.strokeOutlineForWorldTransform({});
    QVERIFY(!solidOutline.isEmpty());
    QVERIFY(dashedOutline.elementCount() > solidOutline.elementCount());

    VectorLayerData data;
    data.objects = {line};
    data.normalise();
    QCOMPARE(data.schema, VectorLayerData::CurrentSchema);
    QCOMPARE(VectorLayerData::CurrentSchema, 8u);
    bool jsonOk = false;
    const QJsonObject encoded = data.toJson(&jsonOk);
    QVERIFY(jsonOk);
    bool decodedOk = false;
    const VectorLayerData decoded = VectorLayerData::fromJson(encoded, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(decoded, data);
    const VectorStroke decodedStroke = decoded.objects.constFirst().stroke;
    QCOMPARE(decodedStroke.pattern, VectorStrokePattern::Dashed);
    QCOMPARE(decodedStroke.dashLength, 18.0);
    QCOMPARE(decodedStroke.gapLength, 7.0);
    QCOMPARE(decodedStroke.dashOffset, 3.0);
    QCOMPARE(decodedStroke.cap, VectorStrokeCap::Round);

    QJsonObject dishonestSchema = encoded;
    dishonestSchema.insert(QStringLiteral("schema"), 3);
    VectorLayerData::fromJson(dishonestSchema, &decodedOk);
    QVERIFY(!decodedOk);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(240, 80);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QLineF documentLine(line.lineStart, line.lineEnd);
    const QUuid layerId = document.addVectorShape(
        VectorShapeType::Line, QRectF(documentLine.p1(), documentLine.p2()).normalized(),
        QColor(Qt::transparent), {}, 0.0, documentLine);
    QVERIFY(!layerId.isNull());
    QVERIFY(document.updateLayer(layerId, [line](LayerNode &layer) {
        layer.vectorData.objects.first().stroke = line.stroke;
        ++layer.vectorData.objects.first().revision;
        layer.vectorData.normalise();
    }));

    const QString projectPath = directory.filePath(QStringLiteral("dashed-v11.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    QFile projectFile(projectPath);
    QVERIFY(projectFile.open(QIODevice::ReadOnly));
    const QJsonDocument projectJson = QJsonDocument::fromJson(projectFile.readAll());
    projectFile.close();
    QVERIFY(projectJson.isObject());
    QCOMPARE(projectJson.object().value(QStringLiteral("version")).toInt(), 15);

    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    QCOMPARE(restored.layerById(layerId).vectorData.objects.constFirst().stroke,
             line.stroke);

    QJsonObject dishonestProject = projectJson.object();
    dishonestProject.insert(QStringLiteral("version"), 10);
    const QString dishonestPath = directory.filePath(
        QStringLiteral("illegal-dashed-v10.vfxphoto"));
    QFile dishonestFile(dishonestPath);
    QVERIFY(dishonestFile.open(QIODevice::WriteOnly));
    const QByteArray bytes = QJsonDocument(dishonestProject).toJson(QJsonDocument::Compact);
    QCOMPARE(dishonestFile.write(bytes), static_cast<qint64>(bytes.size()));
    dishonestFile.close();
    PhotoDocument rejected;
    QVERIFY(!rejected.loadProject(dishonestPath, &error));
    QVERIFY(error.contains(QStringLiteral("version-11"), Qt::CaseInsensitive));
}

void CoreTests::vectorSnapPointsExposeSemanticVertices()
{
    VectorShape line;
    line.type = VectorShapeType::Line;
    line.lineStart = QPointF(2.0, 4.0);
    line.lineEnd = QPointF(18.0, 12.0);
    line.stroke.enabled = true;
    line.normalise();
    const QVector<QPointF> linePoints = line.snapPoints();
    QCOMPARE(linePoints.size(), 3);
    QCOMPARE(linePoints.at(0), line.lineStart);
    QCOMPARE(linePoints.at(1), line.lineEnd);
    QCOMPARE(linePoints.at(2), QPointF(10.0, 8.0));

    VectorShape polygon;
    polygon.type = VectorShapeType::Polygon;
    polygon.bounds = QRectF(0.0, 0.0, 40.0, 20.0);
    polygon.polygonSides = 6;
    polygon.vertexRotationDegrees = 0.0;
    polygon.normalise();
    const QVector<QPointF> polygonPoints = polygon.snapPoints();
    QCOMPARE(polygonPoints.size(), 7);
    QCOMPARE(polygonPoints.constLast(), polygon.bounds.center());
    QVERIFY(QLineF(polygonPoints.constFirst(), QPointF(40.0, 10.0)).length()
            <= 1.0e-9);

    VectorShape star;
    star.type = VectorShapeType::Star;
    star.bounds = QRectF(10.0, 20.0, 60.0, 40.0);
    star.polygonSides = 5;
    star.starInnerRatio = 0.4;
    star.vertexRotationDegrees = -90.0;
    star.transform = QTransform::fromTranslate(3.0, -2.0);
    star.normalise();
    QTransform world;
    world.translate(100.0, 50.0);
    const QVector<QPointF> starPoints = star.snapPoints(world);
    QCOMPARE(starPoints.size(), 11);
    QCOMPARE(starPoints.constLast(), world.map(star.transform.map(star.bounds.center())));

    VectorLayerData layer;
    layer.objects = {line, polygon, star};
    layer.normalise();
    QCOMPARE(layer.snapPoints(world).size(), 21);
}

void CoreTests::vectorLayerCopyPreservesWorldPlacementAndRegeneratesIds()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(320, 180);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));

    const QUuid sourceId = document.addVectorShape(
        VectorShapeType::Star, QRectF(-30.0, 15.0, 80.0, 60.0),
        QColor(80, 160, 230, 200));
    QVERIFY(!sourceId.isNull());
    QVERIFY(document.addMask(sourceId));
    QVERIFY(document.updateLayer(sourceId, [](LayerNode &layer) {
        layer.name = QStringLiteral("Clipboard Star");
        layer.opacity = 0.64;
        layer.blendMode = BlendMode::Screen;
        VectorShape &shape = layer.vectorData.objects.first();
        shape.polygonSides = 8;
        shape.starInnerRatio = 0.31;
        shape.vertexRotationDegrees = 12.0;
        shape.stroke.enabled = true;
        shape.stroke.width = 5.0;
        shape.stroke.alignment = VectorStrokeAlignment::Outside;
        ++shape.revision;
        layer.vectorData.normalise();
    }));
    const LayerNode source = document.layerById(sourceId);
    const QUuid sourceObjectId = source.vectorData.objects.constFirst().id;

    const QUuid groupId = document.addGroup();
    QVERIFY(!groupId.isNull());
    QVERIFY(document.updateLayer(groupId, [](LayerNode &group) {
        group.transform = QTransform::fromTranslate(37.0, -18.0)
            * QTransform::fromScale(1.25, 0.8);
    }));

    QTransform desiredWorld;
    desiredWorld.translate(145.0, 42.0);
    desiredWorld.rotate(17.0);
    const QUuid copyId = document.insertVectorLayerCopy(source, desiredWorld, groupId);
    QVERIFY(!copyId.isNull());
    QVERIFY(copyId != sourceId);
    const LayerNode copy = document.layerById(copyId);
    QCOMPARE(copy.type, LayerType::Vector);
    QCOMPARE(copy.name, source.name);
    QCOMPARE(copy.opacity, source.opacity);
    QCOMPARE(copy.blendMode, source.blendMode);
    QVERIFY(copy.hasMask());
    QCOMPARE(copy.vectorData.objects.size(), 1);
    QVERIFY(copy.vectorData.objects.constFirst().id != sourceObjectId);
    QCOMPARE(copy.vectorData.objects.constFirst().polygonSides, 8);
    QCOMPARE(copy.vectorData.objects.constFirst().starInnerRatio, 0.31);
    QVERIFY(transformsClose(document.layerWorldTransform(copyId), desiredWorld));

    QUuid parentId;
    int index = -1;
    QVERIFY(document.layerPlacement(copyId, &parentId, &index));
    QCOMPARE(parentId, groupId);
    QVERIFY(index >= 0);
}

void CoreTests::imageSizeScalesLineGeometryAndStrokeWithoutRasterising()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(100, 80);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QLineF originalLine(QPointF(10.0, 60.0), QPointF(70.0, 15.0));
    const QUuid lineId = document.addVectorShape(
        VectorShapeType::Line,
        QRectF(originalLine.p1(), originalLine.p2()).normalized(),
        QColor(Qt::red),
        {},
        0.0,
        originalLine);
    QVERIFY(!lineId.isNull());
    QVERIFY(document.updateLayer(lineId, [](LayerNode &layer) {
        VectorShape &shape = layer.vectorData.objects.first();
        shape.stroke.width = 6.0;
        shape.stroke.cap = VectorStrokeCap::Square;
        shape.stroke.pattern = VectorStrokePattern::Dashed;
        shape.stroke.dashLength = 12.0;
        shape.stroke.gapLength = 4.0;
        shape.stroke.dashOffset = 2.0;
        shape.stroke.startArrowhead = VectorArrowheadType::Diamond;
        shape.stroke.endArrowhead = VectorArrowheadType::Triangle;
        shape.stroke.startArrowScale = 1.3;
        shape.stroke.endArrowScale = 1.8;
        layer.vectorData.featherRadius = 8.0;
        ++shape.revision;
        layer.vectorData.normalise();
    }));

    ImageSizeRequest request;
    request.pixelSize = QSize(250, 120);
    request.method = ImageResampleMethod::NearestNeighbour;
    ImageSizeResult result;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));

    LayerNode scaled;
    std::function<bool(const QVector<LayerNode> &)> find;
    find = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (layer.id == lineId) {
                scaled = layer;
                return true;
            }
            if (find(layer.children)) return true;
        }
        return false;
    };
    QVERIFY(find(result.layers));
    QCOMPARE(scaled.type, LayerType::Vector);
    QVERIFY(scaled.rasterImage.isNull());
    const VectorShape shape = scaled.vectorData.objects.constFirst();
    QCOMPARE(shape.type, VectorShapeType::Line);
    QCOMPARE(shape.lineStart, QPointF(25.0, 90.0));
    QCOMPARE(shape.lineEnd, QPointF(175.0, 22.5));
    // Image Size changes document resolution, so document-pixel stroke widths
    // and vector Feather scale by the smaller axis factor (1.5 here) rather
    // than flattening or being left at the old pixel radius.
    QCOMPARE(shape.stroke.width, 9.0);
    QCOMPARE(scaled.vectorData.featherRadius, 12.0);
    QCOMPARE(shape.stroke.cap, VectorStrokeCap::Square);
    QCOMPARE(shape.stroke.pattern, VectorStrokePattern::Dashed);
    QCOMPARE(shape.stroke.dashLength, 18.0);
    QCOMPARE(shape.stroke.gapLength, 6.0);
    QCOMPARE(shape.stroke.dashOffset, 3.0);
    QCOMPARE(shape.stroke.startArrowhead, VectorArrowheadType::Diamond);
    QCOMPARE(shape.stroke.endArrowhead, VectorArrowheadType::Triangle);
    QCOMPARE(shape.stroke.startArrowScale, 1.3);
    QCOMPARE(shape.stroke.endArrowScale, 1.8);
    QCOMPARE(shape.bounds, QRectF(shape.lineStart, shape.lineEnd).normalized());
}

void CoreTests::vectorLayersCompositeThroughMasksGroupsAndTransforms()
{
    const QSize size(64, 48);
    QImage source(size, QImage::Format_RGBA8888);
    source.fill(Qt::transparent);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));

    LayerNode vector;
    vector.type = LayerType::Vector;
    vector.transform = QTransform::fromTranslate(10.0, 8.0);
    vector.opacity = 0.5;
    VectorShape shape;
    shape.type = VectorShapeType::Rectangle;
    shape.bounds = QRectF(0.0, 0.0, 24.0, 20.0);
    shape.fill.colour = QColor(220, 40, 20, 255);
    vector.vectorData.objects = {shape};
    vector.vectorData.normalise();

    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(255);
    for (int y = 0; y < mask.height(); ++y) {
        std::memset(mask.scanLine(y), 0, 12);
    }
    vector.maskImage = mask;
    vector.maskReferenceSize = size;
    vector.maskEnabled = true;

    LayerNode group;
    group.type = LayerType::Group;
    group.groupCompositeMode = GroupCompositeMode::Isolated;
    group.transform = QTransform::fromTranslate(5.0, 0.0);
    group.children = {vector};

    const QImage isolated = ImageProcessor::renderPreservingHiddenRgb(
        source, {group}, nullptr, size).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(!isolated.isNull());
    QCOMPARE(isolated.pixelColor(19, 15).alpha(), 0);
    const QColor visible = isolated.pixelColor(31, 15);
    QVERIFY(visible.alpha() >= 126 && visible.alpha() <= 129);
    QVERIFY(visible.red() > 210);
    QCOMPARE(visible.green(), 40);

    group.groupCompositeMode = GroupCompositeMode::PassThrough;
    const QImage passThrough = ImageProcessor::renderPreservingHiddenRgb(
        source, {group}, nullptr, size).convertToFormat(QImage::Format_RGBA8888);
    QVERIFY(exactImagesEqual(isolated, passThrough));

    const QRectF bounds = ImageProcessor::contentBounds(
        source, {group}, {group.id}, size);
    QVERIFY(bounds.left() >= 26.0 - 1.0e-6);
    QVERIFY(bounds.right() <= 39.0 + 1.0e-6);
    QVERIFY(bounds.top() >= 8.0 - 1.0e-6);
    QVERIFY(bounds.bottom() <= 28.0 + 1.0e-6);
}

void CoreTests::fillCoverageSupportsContiguousAndGlobalMatching()
{
    QImage source(8, 4, QImage::Format_RGBA8888);
    source.fill(QColor(10, 20, 30, 255));
    for (int y = 0; y < source.height(); ++y) {
        source.setPixelColor(3, y, QColor(200, 210, 220, 255));
    }

    FillCoverageRequest request;
    request.sourceImage = source;
    request.documentSize = source.size();
    request.documentPosition = QPointF(1.5, 1.5);
    request.tolerance = 0;
    request.contiguous = true;
    const FillCoverageResult contiguous = buildFillCoverage(request);
    QVERIFY2(contiguous.succeeded(), qPrintable(contiguous.error));
    QCOMPARE(contiguous.matchedPixelCount, 12);
    QCOMPARE(contiguous.coverage.constScanLine(1)[1], uchar(255));
    QCOMPARE(contiguous.coverage.constScanLine(1)[5], uchar(0));

    request.contiguous = false;
    const FillCoverageResult global = buildFillCoverage(request);
    QVERIFY2(global.succeeded(), qPrintable(global.error));
    QCOMPARE(global.matchedPixelCount, 28);
    QCOMPARE(global.coverage.constScanLine(1)[5], uchar(255));

    source.setPixelColor(2, 1, QColor(14, 24, 34, 255));
    request.sourceImage = source;
    request.contiguous = true;
    request.tolerance = 3;
    const FillCoverageResult strict = buildFillCoverage(request);
    QVERIFY(strict.succeeded());
    QCOMPARE(strict.coverage.constScanLine(1)[2], uchar(0));
    request.tolerance = 4;
    const FillCoverageResult tolerant = buildFillCoverage(request);
    QVERIFY(tolerant.succeeded());
    QCOMPARE(tolerant.coverage.constScanLine(1)[2], uchar(255));
}

void CoreTests::fillCoverageHonoursSelectionAndTransparentEquivalence()
{
    QImage source(5, 3, QImage::Format_RGBA8888);
    source.fill(QColor(9, 17, 25, 0));
    source.setPixelColor(1, 1, QColor(220, 11, 70, 0));
    source.setPixelColor(2, 1, QColor(3, 240, 90, 0));
    source.setPixelColor(3, 1, QColor(50, 60, 70, 255));

    SelectionMask selection(source.size());
    selection.selectNone();
    QImage selected(source.size(), QImage::Format_Grayscale8);
    selected.fill(0);
    selected.scanLine(1)[0] = 255;
    selected.scanLine(1)[1] = 128;
    selected.scanLine(1)[2] = 255;
    QVERIFY(selection.setCoverageImage(source.rect(), selected));

    FillCoverageRequest request;
    request.sourceImage = source;
    request.documentSize = source.size();
    request.documentPosition = QPointF(0.5, 1.5);
    request.tolerance = 0;
    request.contiguous = true;
    request.selectionSnapshot = selection.snapshot();
    const FillCoverageResult result = buildFillCoverage(request);
    QVERIFY2(result.succeeded(), qPrintable(result.error));
    // Fully transparent pixels compare by apparent alpha, not hidden RGB.
    QCOMPARE(result.matchedPixelCount, 3);
    QCOMPARE(result.coverage.constScanLine(1)[0], uchar(255));
    QCOMPARE(result.coverage.constScanLine(1)[1], uchar(128));
    QCOMPARE(result.coverage.constScanLine(1)[2], uchar(255));
    QCOMPARE(result.coverage.constScanLine(1)[3], uchar(0));

    request.documentPosition = QPointF(4.5, 2.5);
    const FillCoverageResult outside = buildFillCoverage(request);
    QVERIFY(!outside.succeeded());
    QVERIFY(outside.error.contains(QStringLiteral("selection"), Qt::CaseInsensitive));
}

void CoreTests::fillApplicationPreservesAlphaChannelsMasksAndSixteenBitData()
{
    QImage source(2, 1, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(7, 19, 31, 0));
    source.setPixelColor(1, 0, QColor(40, 50, 60, 120));
    QImage coverage(source.size(), QImage::Format_Grayscale8);
    coverage.fill(255);

    const FillApplyResult colour = applyFillCoverageCpu(
        source, coverage, FillTarget::RasterPixels, -1,
        QColor(200, 100, 50, 64), false);
    QVERIFY2(colour.succeeded(), qPrintable(colour.error));
    QCOMPARE(colour.image.pixelColor(0, 0), QColor(200, 100, 50, 64));
    QCOMPARE(colour.image.pixelColor(1, 0), QColor(200, 100, 50, 64));

    const FillApplyResult preserve = applyFillCoverageCpu(
        source, coverage, FillTarget::RasterPixels, -1,
        QColor(201, 101, 51, 255), true);
    QVERIFY(preserve.succeeded());
    QCOMPARE(preserve.image.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(preserve.image.pixelColor(0, 0).red(), 201);
    QCOMPARE(preserve.image.pixelColor(1, 0).alpha(), 120);

    QImage featherSource(1, 1, QImage::Format_RGBA8888);
    featherSource.setPixelColor(0, 0, QColor(13, 27, 39, 0));
    QImage featherCoverage(1, 1, QImage::Format_Grayscale8);
    featherCoverage.fill(128);
    const FillApplyResult feathered = applyFillCoverageCpu(
        featherSource, featherCoverage, FillTarget::RasterPixels, -1,
        QColor(200, 100, 50, 255), false);
    QVERIFY(feathered.succeeded());
    QCOMPARE(feathered.image.pixelColor(0, 0), QColor(200, 100, 50, 128));

    QImage transparentCoverage(1, 1, QImage::Format_Grayscale8);
    transparentCoverage.fill(255);
    const FillApplyResult transparentFill = applyFillCoverageCpu(
        source.copy(1, 0, 1, 1), transparentCoverage,
        FillTarget::RasterPixels, -1, QColor(250, 1, 2, 0), false);
    QVERIFY(transparentFill.succeeded());
    QCOMPARE(transparentFill.image.pixelColor(0, 0), QColor(40, 50, 60, 0));

    const FillApplyResult alphaChannel = applyFillCoverageCpu(
        source, coverage, FillTarget::ComponentChannel, 3,
        QColor(128, 128, 128), false);
    QVERIFY(alphaChannel.succeeded());
    QCOMPARE(alphaChannel.image.pixelColor(0, 0), QColor(7, 19, 31, 128));
    QCOMPARE(alphaChannel.image.pixelColor(1, 0), QColor(40, 50, 60, 128));

    QImage mask(3, 1, QImage::Format_Grayscale8);
    mask.fill(10);
    QImage maskCoverage(mask.size(), QImage::Format_Grayscale8);
    maskCoverage.fill(255);
    const FillApplyResult maskResult = applyFillCoverageCpu(
        mask, maskCoverage, FillTarget::Mask, -1,
        QColor(220, 220, 220), false);
    QVERIFY(maskResult.succeeded());
    QCOMPARE(maskResult.image.constScanLine(0)[2], uchar(220));
    const QImage compact = compactUniformFillMask(maskResult.image);
    QCOMPARE(compact.size(), QSize(1, 1));
    QCOMPARE(compact.constScanLine(0)[0], uchar(220));

    QImage source64(1, 1, QImage::Format_RGBA64);
    auto *pixel64 = reinterpret_cast<QRgba64 *>(source64.scanLine(0));
    pixel64[0] = QRgba64::fromRgba64(1234, 2345, 3456, 4567);
    QImage coverage64(1, 1, QImage::Format_Grayscale8);
    coverage64.fill(128);
    const QColor desired = QColor::fromRgba64(
        QRgba64::fromRgba64(50000, 40000, 30000, 20000));
    const FillApplyResult result64 = applyFillCoverageCpu(
        source64, coverage64, FillTarget::RasterPixels, -1, desired, false);
    QVERIFY(result64.succeeded());
    const auto *out64 = reinterpret_cast<const QRgba64 *>(result64.image.constScanLine(0));
    const double coverageAmount = 128.0 / 255.0;
    const double beforeAlpha = 4567.0 / 65535.0;
    const double desiredAlpha = 20000.0 / 65535.0;
    const double outputAlpha = beforeAlpha
        + (desiredAlpha - beforeAlpha) * coverageAmount;
    const quint16 expectedAlpha = static_cast<quint16>(
        qRound(outputAlpha * 65535.0));
    const quint16 expectedRed = static_cast<quint16>(qRound(
        (((1234.0 / 65535.0) * beforeAlpha * (1.0 - coverageAmount))
         + ((50000.0 / 65535.0) * desiredAlpha * coverageAmount))
        / outputAlpha * 65535.0));
    QCOMPARE(out64[0].red(), expectedRed);
    QCOMPARE(out64[0].alpha(), expectedAlpha);
}

void CoreTests::tiledFillCpuPathMatchesDirectReference()
{
    QImage source(519, 263, QImage::Format_RGBA8888);
    QImage coverage(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < source.height(); ++y) {
        uchar *src = source.scanLine(y);
        uchar *cov = coverage.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            src[x * 4] = uchar((x * 7 + y * 3) & 255);
            src[x * 4 + 1] = uchar((x * 5 + y * 11) & 255);
            src[x * 4 + 2] = uchar((x * 13 + y * 2) & 255);
            src[x * 4 + 3] = uchar((x + y) % 9 == 0 ? 0 : 211);
            cov[x] = uchar((x * 17 + y * 19) & 255);
        }
    }
    const QColor colour(230, 40, 140, 90);
    const FillApplyResult direct = applyFillCoverageCpu(
        source, coverage, FillTarget::RasterPixels, -1, colour, false);
    QVERIFY(direct.succeeded());

    TiledCanvasEngine engine(nullptr);
    const auto tiled = engine.applyFillCoverage(source,
                                                 coverage,
                                                 QUuid::createUuid(),
                                                 7,
                                                 FillTarget::RasterPixels,
                                                 -1,
                                                 colour,
                                                 false,
                                                 false);
    QVERIFY2(tiled.error.isEmpty(), qPrintable(tiled.error));
    QVERIFY(tiled.changed());
    QVERIFY(!tiled.usedGpu);
    QVERIFY(exactImagesEqual(direct.image, tiled.image));
    QCOMPARE(direct.changedPixelCount, tiled.changedPixelCount);
    QCOMPARE(direct.affectedRect, tiled.affectedRect);
}

void CoreTests::gradientGeometryEvaluatesAllModes()
{
    const QPointF start(0.0, 0.0);
    const QPointF end(10.0, 0.0);
    QCOMPARE(gradientAmountAt(QPointF(5.0, 0.0), start, end,
                              RasterGradientType::Linear, false),
             0.5);
    QCOMPARE(gradientAmountAt(QPointF(0.0, 5.0), start, end,
                              RasterGradientType::Radial, false),
             0.5);
    QCOMPARE(gradientAmountAt(QPointF(-5.0, 0.0), start, end,
                              RasterGradientType::Reflected, false),
             0.5);
    QCOMPARE(gradientAmountAt(QPointF(2.5, 2.5), start, end,
                              RasterGradientType::Diamond, false),
             0.5);
    QVERIFY(std::abs(gradientAmountAt(QPointF(0.0, 10.0), start, end,
                                      RasterGradientType::Angle, false)
                     - 0.25) < 1.0e-9);
    QVERIFY(std::abs(gradientAmountAt(QPointF(0.0, 10.0), start, end,
                                      RasterGradientType::Angle, true)
                     - 0.75) < 1.0e-9);
    QCOMPARE(gradientAmountAt(QPointF(100.0, 0.0), start, end,
                              RasterGradientType::Linear, false),
             1.0);
    QCOMPARE(gradientAmountAt(QPointF(100.0, 0.0), start, end,
                              RasterGradientType::Linear, true),
             0.0);
}

void CoreTests::gradientApplicationSupportsTargetsSelectionsAndSixteenBit()
{
    QImage source(3, 1, QImage::Format_RGBA8888);
    source.fill(QColor(7, 11, 19, 0));
    QImage coverage(source.size(), QImage::Format_Grayscale8);
    coverage.fill(255);

    GradientApplyRequest colourRequest;
    colourRequest.sourceImage = source;
    colourRequest.selectionCoverage = coverage;
    colourRequest.target = FillTarget::RasterPixels;
    colourRequest.start = QPointF(0.5, 0.5);
    colourRequest.end = QPointF(2.5, 0.5);
    colourRequest.type = RasterGradientType::Linear;
    colourRequest.startColour = QColor(255, 0, 0, 255);
    colourRequest.endColour = QColor(0, 0, 255, 0);
    const GradientApplyResult colour = applyGradientCpu(colourRequest);
    QVERIFY2(colour.succeeded(), qPrintable(colour.error));
    QCOMPARE(colour.image.pixelColor(0, 0), QColor(255, 0, 0, 255));
    const QColor midpoint = colour.image.pixelColor(1, 0);
    QVERIFY(midpoint.red() >= 127 && midpoint.red() <= 128);
    QCOMPARE(midpoint.green(), 0);
    QVERIFY(midpoint.blue() >= 127 && midpoint.blue() <= 128);
    QVERIFY(midpoint.alpha() >= 127 && midpoint.alpha() <= 128);
    // A fully transparent endpoint retains the original hidden RGB.
    QCOMPARE(colour.image.pixelColor(2, 0), QColor(7, 11, 19, 0));

    // A mathematically non-zero Alpha that quantises to zero must still keep
    // the previous hidden RGB rather than replacing it with the gradient colour.
    QImage tinyCoverage(source.size(), QImage::Format_Grayscale8);
    tinyCoverage.fill(1);
    colourRequest.selectionCoverage = tinyCoverage;
    colourRequest.startColour = QColor(200, 100, 50, 1);
    colourRequest.endColour = QColor(200, 100, 50, 1);
    const GradientApplyResult quantisedTransparent = applyGradientCpu(colourRequest);
    QVERIFY(quantisedTransparent.succeeded());
    QCOMPARE(quantisedTransparent.image.pixelColor(1, 0), QColor(7, 11, 19, 0));

    QImage featherCoverage(source.size(), QImage::Format_Grayscale8);
    featherCoverage.fill(0);
    featherCoverage.scanLine(0)[1] = 128;
    colourRequest.selectionCoverage = featherCoverage;
    colourRequest.startColour = QColor(20, 40, 60, 255);
    colourRequest.endColour = QColor(20, 40, 60, 255);
    const GradientApplyResult feathered = applyGradientCpu(colourRequest);
    QVERIFY(feathered.succeeded());
    QCOMPARE(feathered.image.pixelColor(0, 0), source.pixelColor(0, 0));
    const QColor featheredPixel = feathered.image.pixelColor(1, 0);
    QCOMPARE(featheredPixel.red(), 20);
    QCOMPARE(featheredPixel.green(), 40);
    QCOMPARE(featheredPixel.blue(), 60);
    QCOMPARE(featheredPixel.alpha(), 128);

    QImage mask(3, 1, QImage::Format_Grayscale8);
    mask.fill(17);
    GradientApplyRequest maskRequest;
    maskRequest.sourceImage = mask;
    maskRequest.selectionCoverage = coverage;
    maskRequest.target = FillTarget::Mask;
    maskRequest.start = QPointF(0.5, 0.5);
    maskRequest.end = QPointF(2.5, 0.5);
    maskRequest.type = RasterGradientType::Linear;
    maskRequest.startColour = Qt::white;
    maskRequest.endColour = Qt::black;
    const GradientApplyResult maskResult = applyGradientCpu(maskRequest);
    QVERIFY(maskResult.succeeded());
    QCOMPARE(maskResult.image.constScanLine(0)[0], uchar(255));
    QVERIFY(maskResult.image.constScanLine(0)[1] >= 127
            && maskResult.image.constScanLine(0)[1] <= 128);
    QCOMPARE(maskResult.image.constScanLine(0)[2], uchar(0));

    GradientApplyRequest channelRequest = colourRequest;
    channelRequest.sourceImage = QImage(3, 1, QImage::Format_RGBA8888);
    channelRequest.sourceImage.fill(QColor(10, 20, 30, 40));
    channelRequest.selectionCoverage = coverage;
    channelRequest.target = FillTarget::ComponentChannel;
    channelRequest.componentIndex = 3;
    channelRequest.startColour = Qt::white;
    channelRequest.endColour = Qt::black;
    const GradientApplyResult channel = applyGradientCpu(channelRequest);
    QVERIFY(channel.succeeded());
    QCOMPARE(channel.image.pixelColor(0, 0), QColor(10, 20, 30, 255));
    QCOMPARE(channel.image.pixelColor(2, 0), QColor(10, 20, 30, 0));

    QImage source64(2, 1, QImage::Format_RGBA64);
    auto *pixels64 = reinterpret_cast<QRgba64 *>(source64.scanLine(0));
    pixels64[0] = QRgba64::fromRgba64(1000, 2000, 3000, 4000);
    pixels64[1] = QRgba64::fromRgba64(5000, 6000, 7000, 8000);
    QImage coverage64(source64.size(), QImage::Format_Grayscale8);
    coverage64.fill(255);
    GradientApplyRequest request64;
    request64.sourceImage = source64;
    request64.selectionCoverage = coverage64;
    request64.target = FillTarget::RasterPixels;
    request64.start = QPointF(0.5, 0.5);
    request64.end = QPointF(1.5, 0.5);
    request64.startColour = QColor::fromRgba64(
        QRgba64::fromRgba64(60000, 50000, 40000, 30000));
    request64.endColour = QColor::fromRgba64(
        QRgba64::fromRgba64(10000, 20000, 30000, 65535));
    const GradientApplyResult result64 = applyGradientCpu(request64);
    QVERIFY(result64.succeeded());
    QCOMPARE(result64.image.format(), QImage::Format_RGBA64);
    const auto *out64 = reinterpret_cast<const QRgba64 *>(result64.image.constScanLine(0));
    const QRgba64 expectedStart64 = request64.startColour.rgba64();
    const QRgba64 expectedEnd64 = request64.endColour.rgba64();
    QCOMPARE(out64[0].red(), expectedStart64.red());
    QCOMPARE(out64[0].green(), expectedStart64.green());
    QCOMPARE(out64[0].blue(), expectedStart64.blue());
    QCOMPARE(out64[0].alpha(), expectedStart64.alpha());
    QCOMPARE(out64[1].red(), expectedEnd64.red());
    QCOMPARE(out64[1].green(), expectedEnd64.green());
    QCOMPARE(out64[1].blue(), expectedEnd64.blue());
    QCOMPARE(out64[1].alpha(), expectedEnd64.alpha());
}

void CoreTests::tiledGradientCpuPathMatchesDirectReference()
{
    QImage source(519, 263, QImage::Format_RGBA8888);
    QImage coverage(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < source.height(); ++y) {
        uchar *src = source.scanLine(y);
        uchar *cov = coverage.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            src[x * 4] = uchar((x * 7 + y * 3) & 255);
            src[x * 4 + 1] = uchar((x * 5 + y * 11) & 255);
            src[x * 4 + 2] = uchar((x * 13 + y * 2) & 255);
            src[x * 4 + 3] = uchar((x + y) % 11 == 0 ? 0 : 219);
            cov[x] = uchar((x * 17 + y * 19) & 255);
        }
    }
    GradientApplyRequest request;
    request.sourceImage = source;
    request.selectionCoverage = coverage;
    request.target = FillTarget::RasterPixels;
    request.start = QPointF(73.25, 41.75);
    request.end = QPointF(448.5, 219.25);
    request.type = RasterGradientType::Diamond;
    request.startColour = QColor(230, 40, 140, 230);
    request.endColour = QColor(15, 210, 80, 35);
    request.reverse = true;
    const GradientApplyResult direct = applyGradientCpu(request);
    QVERIFY2(direct.succeeded(), qPrintable(direct.error));

    TiledCanvasEngine engine(nullptr);
    const auto tiled = engine.applyGradient(request,
                                            QUuid::createUuid(),
                                            9,
                                            false);
    QVERIFY2(tiled.error.isEmpty(), qPrintable(tiled.error));
    QVERIFY(tiled.changed());
    QVERIFY(!tiled.usedGpu);
    QVERIFY(exactImagesEqual(direct.image, tiled.image));
    QCOMPARE(direct.changedPixelCount, tiled.changedPixelCount);
    QCOMPARE(direct.affectedRect, tiled.affectedRect);
}

void CoreTests::imageSizeScalesVectorGeometryWithoutRasterising()
{
    PhotoDocument document;
    NewDocumentSettings settings;
    settings.pixelSize = QSize(100, 50);
    settings.backgroundColour = QColor(0, 0, 0, 0);
    QString error;
    QVERIFY2(document.createNewDocument(settings, &error), qPrintable(error));
    const QUuid vectorId = document.addVectorShape(
        VectorShapeType::RoundedRectangle,
        QRectF(10.0, 5.0, 20.0, 10.0), QColor(80, 140, 220, 190), {}, 4.0);
    QVERIFY(!vectorId.isNull());
    QVERIFY(document.updateLayer(vectorId, [](LayerNode &layer) {
        layer.transform = QTransform::fromTranslate(5.0, 2.0);
        VectorShape &shape = layer.vectorData.objects.first();
        shape.cornerRadii = VectorCornerRadii {1.0, 2.0, 3.0, 4.0};
        shape.cornerRadiiLinked = false;
        shape.transform = QTransform::fromTranslate(3.0, 1.0);
        ++shape.revision;
        layer.vectorData.normalise();
    }));

    ImageSizeRequest request;
    request.pixelSize = QSize(200, 150);
    request.method = ImageResampleMethod::NearestNeighbour;
    ImageSizeResult result;
    QVERIFY2(buildImageSizeResult(document, request, &result, nullptr, &error),
             qPrintable(error));
    QCOMPARE(result.canvasImage.size(), QSize(200, 150));

    LayerNode scaled;
    std::function<bool(const QVector<LayerNode> &)> find;
    find = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (layer.id == vectorId) {
                scaled = layer;
                return true;
            }
            if (find(layer.children)) return true;
        }
        return false;
    };
    QVERIFY(find(result.layers));
    QCOMPARE(scaled.type, LayerType::Vector);
    QVERIFY(scaled.rasterImage.isNull());
    QVERIFY(scaled.vectorData.isSafe());
    const VectorShape scaledShape = scaled.vectorData.objects.constFirst();
    QCOMPARE(scaledShape.bounds, QRectF(20.0, 15.0, 40.0, 30.0));
    QCOMPARE(scaledShape.cornerRadii.topLeft, 2.0);
    QCOMPARE(scaledShape.cornerRadii.topRight, 4.0);
    QCOMPARE(scaledShape.cornerRadii.bottomRight, 6.0);
    QCOMPARE(scaledShape.cornerRadii.bottomLeft, 8.0);
    QVERIFY(!scaledShape.cornerRadiiLinked);
    QVERIFY(transformsClose(scaled.transform,
                            QTransform::fromTranslate(10.0, 6.0)));
    QVERIFY(transformsClose(scaledShape.transform,
                            QTransform::fromTranslate(6.0, 3.0)));

    QVERIFY2(document.replaceStructuralState(result.canvasImage,
                                             result.layers,
                                             result.selection,
                                             result.horizontalGuides,
                                             result.verticalGuides,
                                             &error),
             qPrintable(error));
    const LayerNode committed = document.layerById(vectorId);
    QVERIFY(committed.rasterImage.isNull());
    QCOMPARE(committed.vectorData, scaled.vectorData);
    const QImage rendered = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers(), nullptr,
        document.sourceImage().size());
    QVERIFY(!rendered.isNull());
    QVERIFY(rendered.pixelColor(45, 30).alpha() > 0);
}

QTEST_APPLESS_MAIN(CoreTests)


#include "test_core.moc"
