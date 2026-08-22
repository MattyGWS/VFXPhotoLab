#include "ImageCanvas.h"
#include "PixelSnapping.h"
#include "DisplayColourManagement.h"
#include "AppStyle.h"
#include "gpu/RenderBackend.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QColorSpace>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QList>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QLineF>
#include <QResizeEvent>
#include <QRegion>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizeF>
#include <QUrl>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace vfx {
namespace {

constexpr double GuideHitTolerance = 7.0;
constexpr double GuideSnapTolerance = 8.0;
constexpr double TransformSnapReleasePadding = 4.0;
constexpr double FreehandLassoSampleDistance = 2.0;
constexpr double FreehandLassoSimplifyDistance = 0.75;
constexpr double PolygonalLassoCloseDistance = 8.0;

double pointSegmentDistance(const QPointF &point,
                            const QPointF &segmentStart,
                            const QPointF &segmentEnd)
{
    const QPointF segment = segmentEnd - segmentStart;
    const double lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 1.0e-12) {
        return QLineF(point, segmentStart).length();
    }
    const double projection = std::clamp(
        QPointF::dotProduct(point - segmentStart, segment) / lengthSquared,
        0.0,
        1.0);
    return QLineF(point, segmentStart + segment * projection).length();
}

void sortAndDeduplicate(QVector<double> &guides)
{
    std::sort(guides.begin(), guides.end());
    QVector<double> unique;
    unique.reserve(guides.size());
    for (const double guide : guides) {
        if (unique.isEmpty() || std::abs(unique.constLast() - guide) > 1.0e-6) {
            unique.push_back(guide);
        }
    }
    guides = std::move(unique);
}

double niceMajorStep(const double pixelsPerDocumentUnit)
{
    if (pixelsPerDocumentUnit <= 0.0) {
        return 100.0;
    }

    const double rawStep = std::max(1.0, 82.0 / pixelsPerDocumentUnit);
    const double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double fraction = rawStep / magnitude;
    if (fraction <= 1.0) {
        return magnitude;
    }
    if (fraction <= 2.0) {
        return 2.0 * magnitude;
    }
    if (fraction <= 5.0) {
        return 5.0 * magnitude;
    }
    return 10.0 * magnitude;
}

QString rulerLabel(const double value)
{
    return QString::number(qRound64(value));
}

QRectF pixelAlignedCropFrame(const QRectF &frame)
{
    const QRectF normalised = frame.normalized();
    return QRectF(qRound(normalised.x()),
                  qRound(normalised.y()),
                  std::max(1, qRound(normalised.width())),
                  std::max(1, qRound(normalised.height())));
}


bool isUsableTransformQuad(const QPolygonF &quad)
{
    if (quad.size() != 4) {
        return false;
    }
    double orientation = 0.0;
    for (int index = 0; index < 4; ++index) {
        const QPointF point = quad.at(index);
        const QPointF next = quad.at((index + 1) % 4);
        const QPointF following = quad.at((index + 2) % 4);
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())
            || QLineF(point, next).length() <= 1.0e-5) {
            return false;
        }
        const QPointF edgeA = next - point;
        const QPointF edgeB = following - next;
        const double cross = edgeA.x() * edgeB.y() - edgeA.y() * edgeB.x();
        if (!std::isfinite(cross) || std::abs(cross) <= 1.0e-7) {
            return false;
        }
        if (orientation == 0.0) {
            orientation = cross;
        } else if ((orientation > 0.0) != (cross > 0.0)) {
            return false;
        }
    }
    return true;
}

bool transformHasContinuousProjectiveDomain(const QTransform &transform,
                                            const QPolygonF &source)
{
    double sign = 0.0;
    for (const QPointF &point : source) {
        const double denominator = transform.m13() * point.x()
            + transform.m23() * point.y() + transform.m33();
        if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-9) {
            return false;
        }
        if (sign == 0.0) {
            sign = denominator;
        } else if ((sign > 0.0) != (denominator > 0.0)) {
            return false;
        }
    }
    return true;
}

} // namespace

class CanvasRuler final : public QWidget {
public:
    CanvasRuler(const Qt::Orientation orientation, ImageCanvas *canvas)
        : QWidget(canvas)
        , m_orientation(orientation)
        , m_canvas(canvas)
    {
        setMouseTracking(true);
        setCursor(orientation == Qt::Horizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        m_canvas->paintRuler(painter, m_orientation, rect());
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_canvas->hasImage()) {
            event->ignore();
            return;
        }

        const Qt::Orientation guideOrientation = m_orientation;
        const double position = m_orientation == Qt::Horizontal
            ? event->position().y()
            : event->position().x();
        m_dragging = true;
        m_canvas->beginGuideFromRuler(guideOrientation, position);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            event->ignore();
            return;
        }
        const double position = m_orientation == Qt::Horizontal
            ? event->position().y()
            : event->position().x();
        m_canvas->updateGuideFromRuler(position);
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_dragging || event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }

        m_dragging = false;
        const bool enteredCanvas = m_orientation == Qt::Horizontal
            ? event->position().y() > height()
            : event->position().x() > width();
        m_canvas->finishGuideFromRuler(enteredCanvas);
        event->accept();
    }

private:
    Qt::Orientation m_orientation;
    ImageCanvas *m_canvas = nullptr;
    bool m_dragging = false;
};

ImageCanvas::ImageCanvas(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setCursor(m_toolCursor);
    horizontalScrollBar()->setSingleStep(48);
    verticalScrollBar()->setSingleStep(48);
    // Scroll bars remain an internal panning mechanism. The canvas itself is
    // navigated directly, so visible scroll bars would only consume workspace.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_horizontalRuler = new CanvasRuler(Qt::Horizontal, this);
    m_verticalRuler = new CanvasRuler(Qt::Vertical, this);
    m_rulerCorner = new QWidget(this);
    m_rulerCorner->setObjectName(QStringLiteral("RulerCorner"));

    m_presentationSettleTimer.setSingleShot(true);
    m_presentationSettleTimer.setInterval(140);
    connect(&m_presentationSettleTimer, &QTimer::timeout, this, [this] {
        notifyPresentationViewportChanged(true);
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        viewport()->update();
        updateRulers();
        notifyPresentationViewportChanged(false);
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        viewport()->update();
        updateRulers();
        notifyPresentationViewportChanged(false);
    });

    m_selectionAntsTimer.setInterval(90);
    connect(&m_selectionAntsTimer, &QTimer::timeout, this, [this] {
        const QPainterPath &displayPath = m_selectionPreviewActive
            ? m_selectionPreviewBoundaryPath : m_selectionBoundaryPath;
        if (!m_selectionEdgesVisible || displayPath.isEmpty()
            || (!m_selectionActive && !m_selectionPreviewActive)) {
            m_selectionAntsTimer.stop();
            return;
        }
        m_selectionAntsPhase = (m_selectionAntsPhase + 1) % 16;
        viewport()->update();
    });

    setRulersVisible(true);
}

QImage ImageCanvas::displayManagedCopy(const QImage &image) const
{
    if (image.isNull() || !m_displayColourTransform
        || m_displayColourTransform->isIdentity()) {
        return {};
    }

    // The CPU transform remains authoritative. Use the validated WGSL path
    // when available, then fall back per surface without changing document
    // pixels or the raw presentation cache.
    QString gpuError;
    QImage converted = RenderBackend::instance().applyDisplayColourTransform(
        image, *m_displayColourTransform, nullptr, &gpuError);
    if (!converted.isNull()) {
        return converted;
    }

    converted = image;
    QString cpuError;
    if (!m_displayColourTransform->apply(&converted, nullptr, &cpuError)) {
        return image;
    }
    return converted;
}

void ImageCanvas::rebuildDisplayPresentation()
{
    m_displayImage = displayManagedCopy(m_image);
    m_displayLiveStrokeImage = displayManagedCopy(m_liveStrokeImage);
    m_displayTransformBackground = displayManagedCopy(m_transformBackground);
    m_displayTransformForeground = displayManagedCopy(m_transformForeground);
    m_displayTransformCompositePreview = displayManagedCopy(m_transformCompositePreview);
    for (auto tile = m_presentationTiles.begin(); tile != m_presentationTiles.end(); ++tile) {
        tile->displayImage = displayManagedCopy(tile->image);
    }
    viewport()->update();
}

void ImageCanvas::setDisplayColourTransform(
    std::shared_ptr<const DisplayColourTransform> transform)
{
    const QByteArray previous = displayColourTransformFingerprint();
    const QByteArray next = transform ? transform->fingerprint() : QByteArray();
    m_displayColourTransform = std::move(transform);
    if (previous != next || m_displayImage.isNull() != m_image.isNull()) {
        rebuildDisplayPresentation();
    }
}

QByteArray ImageCanvas::displayColourTransformFingerprint() const
{
    return m_displayColourTransform ? m_displayColourTransform->fingerprint()
                                    : QByteArray();
}

void ImageCanvas::setImage(const QImage &image, const QSize &documentSize)
{
    const bool hadImage = hasImage();
    m_image = image;
    m_displayImage = displayManagedCopy(m_image);
    m_presentationTiles.clear();
    m_authoritativePreviewCoverage = {};
    m_presentationGeneration = 0;
    m_presentationRequestSerial = 0;
    m_liveStrokeImage = {};
    m_displayLiveStrokeImage = {};
    m_maskOverlayImage = {};
    m_treatmentOverlayImage = {};
    m_documentSize = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : image.size();
    clearTransformPreview();
    if (!hadImage || m_fitMode) {
        fitToView();
    } else {
        const QSignalBlocker horizontalBlock(horizontalScrollBar());
        const QSignalBlocker verticalBlock(verticalScrollBar());
        updateScrollBars();
        viewport()->update();
        updateRulers();
        notifyPresentationViewportChanged(false);
    }
}

void ImageCanvas::beginTiledPresentation(const QImage &fallbackImage,
                                         const QSize &documentSize,
                                         const quint64 generation,
                                         const bool preserveTransientTiles)
{
    if (fallbackImage.isNull()) {
        return;
    }

    const bool hadImage = hasImage();
    const bool samePreviewSize = hadImage && m_image.size() == fallbackImage.size();
    if (generation != m_presentationGeneration) {
        if (!samePreviewSize) {
            m_image = fallbackImage;
            m_displayImage = displayManagedCopy(m_image);
        }
        // Continuous CPU spatial previews retain their last complete frame
        // while the next generation renders, rather than exposing the older
        // backing image between slider events. Blur may retain coarse tiles;
        // detail-sensitive sharpen previews commit level-0 pixels directly.
        // Size changes and ordinary presentation generations still clear all
        // transient tiles.
        if (!preserveTransientTiles || !samePreviewSize) {
            m_presentationTiles.clear();
        }
        m_authoritativePreviewCoverage = {};
        m_presentationGeneration = generation;
        m_presentationRequestSerial = 0;
    }
    if (m_image.isNull()) {
        m_image = fallbackImage;
    }
    m_image = m_image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_image.setColorSpace(fallbackImage.colorSpace());
    m_displayImage = displayManagedCopy(m_image);
    m_documentSize = documentSize.isValid() && !documentSize.isEmpty()
        ? documentSize
        : fallbackImage.size();
    clearTransformPreview();
    if (!hadImage || !samePreviewSize || m_fitMode) {
        fitToView();
    } else {
        const QSignalBlocker horizontalBlock(horizontalScrollBar());
        const QSignalBlocker verticalBlock(verticalScrollBar());
        updateScrollBars();
        viewport()->update();
        updateRulers();
        notifyPresentationViewportChanged(false);
    }
}

void ImageCanvas::beginTiledPresentationRequest(const quint64 generation,
                                                const quint64 requestSerial)
{
    if (generation != m_presentationGeneration
        || requestSerial < m_presentationRequestSerial) {
        return;
    }
    m_presentationRequestSerial = requestSerial;
}

void ImageCanvas::clearTransientPresentationTiles()
{
    if (m_presentationTiles.isEmpty()) {
        return;
    }
    m_presentationTiles.clear();
    viewport()->update();
}

bool ImageCanvas::updatePresentationTile(const QRect &basePreviewRect,
                                         const QImage &tileImage,
                                         const int level,
                                         const quint64 generation,
                                         const quint64 requestSerial)
{
    PresentationTileUpdate update;
    update.basePreviewRect = basePreviewRect;
    update.image = tileImage;
    update.level = level;
    return updatePresentationTiles({update}, generation, requestSerial);
}

bool ImageCanvas::updatePresentationTiles(const QVector<PresentationTileUpdate> &tiles,
                                          const quint64 generation,
                                          const quint64 requestSerial)
{
    if (tiles.isEmpty()
        || generation != m_presentationGeneration
        || requestSerial != m_presentationRequestSerial
        || m_image.isNull()) {
        return false;
    }

    struct PreparedUpdate {
        QRect clipped;
        QImage image;
        QImage displayImage;
        int level = 0;
    };
    QVector<PreparedUpdate> prepared;
    prepared.reserve(tiles.size());
    for (const PresentationTileUpdate &tile : tiles) {
        if (tile.image.isNull() || tile.basePreviewRect.isEmpty()) {
            return false;
        }
        const QRect clipped = tile.basePreviewRect.intersected(m_image.rect());
        if (clipped.isEmpty()) {
            return false;
        }
        prepared.push_back({clipped, tile.image, displayManagedCopy(tile.image),
                            std::max(0, tile.level)});
    }

    // Validate the whole batch before mutating the backing image. Content-edit
    // generations use this as an atomic viewport swap: the previous complete
    // sharp result remains visible until every replacement level-0 tile is
    // ready, and then all replacements are committed before Qt can repaint.
    QRegion dirtyPreviewRegion;
    QPainter backingPainter;
    QPainter displayBackingPainter;
    const bool managedPresentation = m_displayColourTransform
        && !m_displayColourTransform->isIdentity();
    const bool hasAuthoritative = std::any_of(
        prepared.cbegin(), prepared.cend(), [](const PreparedUpdate &tile) {
            return tile.level == 0;
        });
    if (hasAuthoritative) {
        if (!backingPainter.begin(&m_image)) {
            return false;
        }
        backingPainter.setCompositionMode(QPainter::CompositionMode_Source);
        backingPainter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        if (managedPresentation) {
            if (m_displayImage.isNull() || m_displayImage.size() != m_image.size()) {
                m_displayImage = displayManagedCopy(m_image);
            }
            if (m_displayImage.isNull()
                || !displayBackingPainter.begin(&m_displayImage)) {
                backingPainter.end();
                return false;
            }
            displayBackingPainter.setCompositionMode(QPainter::CompositionMode_Source);
            displayBackingPainter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        } else {
            m_displayImage = {};
        }
    }

    for (const PreparedUpdate &tile : std::as_const(prepared)) {
        dirtyPreviewRegion = dirtyPreviewRegion.united(QRegion(tile.clipped));
        if (tile.level == 0) {
            backingPainter.drawImage(tile.clipped, tile.image);
            if (managedPresentation) {
                displayBackingPainter.drawImage(tile.clipped, tile.displayImage);
            }
            m_authoritativePreviewCoverage = m_authoritativePreviewCoverage.united(
                QRegion(tile.clipped));
            continue;
        }

        // Coarse levels are navigation-only and must never degrade the sharp
        // backing image used by edits, transform commits or subsequent strokes.
        const QString key = presentationTileKey(tile.clipped, tile.level);
        PresentationTile record;
        record.basePreviewRect = tile.clipped;
        record.image = tile.image;
        record.displayImage = tile.displayImage;
        record.level = tile.level;
        record.lastUseSerial = ++m_presentationUseSerial;
        m_presentationTiles.insert(key, std::move(record));
    }
    if (hasAuthoritative) {
        backingPainter.end();
        if (managedPresentation) displayBackingPainter.end();
    }
    trimPresentationTiles();

    QRegion dirtyViewportRegion;
    for (const QRect &rect : dirtyPreviewRegion) {
        dirtyViewportRegion = dirtyViewportRegion.united(
            QRegion(viewportRectForPreviewRegion(rect)));
    }
    viewport()->update(dirtyViewportRegion);
    return true;
}

QRect ImageCanvas::visiblePreviewRegion() const
{
    if (!hasImage()) {
        return {};
    }
    const QRectF target = imageRect();
    const QRectF visibleViewport = target.intersected(QRectF(viewport()->rect()));
    if (visibleViewport.isEmpty()) {
        return {};
    }
    const double sx = m_image.width() / std::max(1.0, target.width());
    const double sy = m_image.height() / std::max(1.0, target.height());
    return QRectF((visibleViewport.left() - target.left()) * sx,
                  (visibleViewport.top() - target.top()) * sy,
                  visibleViewport.width() * sx,
                  visibleViewport.height() * sy)
        .adjusted(-2.0, -2.0, 2.0, 2.0)
        .toAlignedRect()
        .intersected(m_image.rect());
}

void ImageCanvas::clearImage()
{
    m_image = {};
    m_displayImage = {};
    m_presentationTiles.clear();
    m_authoritativePreviewCoverage = {};
    m_presentationGeneration = 0;
    m_presentationRequestSerial = 0;
    m_liveStrokeImage = {};
    m_displayLiveStrokeImage = {};
    m_maskOverlayImage = {};
    m_treatmentOverlayImage = {};
    clearSelectionDisplay();
    clearTransformPreview();
    clearTransformSelection();
    m_documentSize = {};
    m_horizontalGuides.clear();
    m_verticalGuides.clear();
    cancelGuideDrag();
    m_zoom = 1.0;
    m_fitMode = true;
    updateScrollBars();
    viewport()->update();
    updateRulers();
    notifyPresentationViewportChanged(false);
    emit zoomChanged(m_zoom);
}

bool ImageCanvas::hasImage() const
{
    return !m_image.isNull();
}

double ImageCanvas::zoom() const
{
    return m_zoom;
}

bool ImageCanvas::isFitToView() const
{
    return m_fitMode;
}

QPoint ImageCanvas::scrollPosition() const
{
    return QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

void ImageCanvas::setScrollPosition(const QPoint &position)
{
    const QSignalBlocker horizontalBlocker(horizontalScrollBar());
    const QSignalBlocker verticalBlocker(verticalScrollBar());
    horizontalScrollBar()->setValue(position.x());
    verticalScrollBar()->setValue(position.y());
    viewport()->update();
    updateRulers();
    notifyPresentationViewportChanged(true);
}

void ImageCanvas::setZoom(const double zoomValue, const QPointF &anchor)
{
    if (!hasImage()) {
        return;
    }

    const double newZoom = std::clamp(zoomValue, 0.02, 32.0);
    if (qFuzzyCompare(newZoom, m_zoom) && !m_fitMode) {
        return;
    }

    const QPointF viewportAnchor = anchor.isNull()
        ? QPointF(viewport()->rect().center())
        : anchor;
    const QRectF oldRect = imageRect();
    const QPointF imagePoint((viewportAnchor.x() - oldRect.left()) / m_zoom,
                             (viewportAnchor.y() - oldRect.top()) / m_zoom);

    m_zoom = newZoom;
    m_fitMode = false;
    const QSignalBlocker horizontalBlock(horizontalScrollBar());
    const QSignalBlocker verticalBlock(verticalScrollBar());
    updateScrollBars();

    // In free-navigation mode each axis has half a viewport of overscroll.
    // This keeps the zoom anchor stable while allowing either image edge to be
    // brought all the way to the centre of the canvas.
    const double horizontalMargin = viewport()->width() * 0.5;
    const double verticalMargin = viewport()->height() * 0.5;
    horizontalScrollBar()->setValue(
        qRound(horizontalMargin + imagePoint.x() * m_zoom - viewportAnchor.x()));
    verticalScrollBar()->setValue(
        qRound(verticalMargin + imagePoint.y() * m_zoom - viewportAnchor.y()));

    viewport()->update();
    updateRulers();
    notifyPresentationViewportChanged(false);
    emit zoomChanged(m_zoom);
}


void ImageCanvas::zoomOutToPreviousStop()
{
    if (!hasImage()) {
        return;
    }

    constexpr double epsilon = 1.0e-9;
    double target = 0.02;
    if (m_zoom > 0.25 + epsilon) {
        constexpr double regularStep = 0.25;
        target = (std::ceil((m_zoom - epsilon) / regularStep) - 1.0) * regularStep;
    } else if (m_zoom > 0.125 + epsilon) {
        target = 0.125;
    } else if (m_zoom > 0.0625 + epsilon) {
        target = 0.0625;
    }

    setZoom(target);
}

void ImageCanvas::zoomInToNextStop()
{
    if (!hasImage()) {
        return;
    }

    constexpr double epsilon = 1.0e-9;
    double target = 32.0;
    if (m_zoom < 0.0625 - epsilon) {
        target = 0.0625;
    } else if (m_zoom < 0.125 - epsilon) {
        target = 0.125;
    } else if (m_zoom < 0.25 - epsilon) {
        target = 0.25;
    } else {
        constexpr double regularStep = 0.25;
        target = (std::floor((m_zoom + epsilon) / regularStep) + 1.0) * regularStep;
    }

    setZoom(target);
}

void ImageCanvas::fitToView()
{
    if (!hasImage() || viewport()->width() <= 0 || viewport()->height() <= 0) {
        return;
    }

    const double horizontal = (viewport()->width() - 32.0) / m_image.width();
    const double vertical = (viewport()->height() - 32.0) / m_image.height();
    m_zoom = std::clamp(std::min(horizontal, vertical), 0.02, 32.0);
    m_fitMode = true;
    const QSignalBlocker horizontalBlock(horizontalScrollBar());
    const QSignalBlocker verticalBlock(verticalScrollBar());
    updateScrollBars();
    horizontalScrollBar()->setValue(0);
    verticalScrollBar()->setValue(0);
    viewport()->update();
    updateRulers();
    notifyPresentationViewportChanged(false);
    emit zoomChanged(m_zoom);
}

void ImageCanvas::actualPixels()
{
    setZoom(1.0);
}

void ImageCanvas::setToolCursor(const Qt::CursorShape cursor)
{
    m_toolCursor = cursor;
    if (!m_panning && !m_spaceHeld && !m_draggingGuide) {
        if (m_vectorPathEditingEnabled) updateVectorPathCursor();
        else viewport()->setCursor(m_toolCursor);
    }
}

QPointF ImageCanvas::mapDocumentToViewport(const QPointF &documentPosition) const
{
    return documentToViewportTransform().map(documentPosition);
}

QRectF ImageCanvas::mapDocumentRectToViewport(const QRectF &documentRect) const
{
    return documentToViewportTransform().mapRect(documentRect);
}

QTransform ImageCanvas::documentToViewportMapping() const
{
    return documentToViewportTransform();
}

void ImageCanvas::setTextToolHitTestingEnabled(const bool enabled)
{
    m_textToolHitTestingEnabled = enabled;
    if (!enabled) m_textEditingActive = false;
}

void ImageCanvas::setVectorPathEditingEnabled(const bool enabled)
{
    m_vectorPathEditingEnabled = enabled;
    if (!enabled) {
        m_vectorPathPointerDown = false;
        clearVectorPathOverlay();
    }
    viewport()->update();
}

bool ImageCanvas::vectorPathEditingEnabled() const
{
    return m_vectorPathEditingEnabled;
}

void ImageCanvas::setVectorPathOverlay(const QVector<CanvasVectorPathNode> &nodes,
                                       const bool closed,
                                       const QSet<int> &selectedNodes,
                                       const int activeHandleNode,
                                       const QPainterPath &displayPath,
                                       const bool cornerEditing,
                                       const CanvasVectorHover &hover)
{
    m_vectorPathNodes = nodes;
    m_vectorPathClosed = closed;
    m_vectorPathSelectedNodes = selectedNodes;
    m_vectorPathActiveHandleNode = activeHandleNode;
    m_vectorPathDisplayPath = displayPath;
    m_vectorCornerEditing = cornerEditing;
    m_vectorPathHover = hover;
    updateVectorPathCursor();
    viewport()->update();
}

void ImageCanvas::setVectorPathEndpointMarkers(
    const QVector<CanvasVectorEndpointMarker> &markers)
{
    m_vectorPathEndpointMarkers = markers;
    updateVectorPathCursor();
    viewport()->update();
}

void ImageCanvas::updateVectorPathCursor()
{
    if (m_spaceHeld || m_panning) return;
    const bool endpointHovered = std::any_of(
        m_vectorPathEndpointMarkers.cbegin(),
        m_vectorPathEndpointMarkers.cend(),
        [](const CanvasVectorEndpointMarker &marker) { return marker.hovered; });
    if (endpointHovered) {
        viewport()->setCursor(Qt::PointingHandCursor);
        return;
    }
    switch (m_vectorPathHover.part) {
    case CanvasVectorHoverPart::Anchor:
        viewport()->setCursor(Qt::SizeAllCursor);
        return;
    case CanvasVectorHoverPart::InHandle:
    case CanvasVectorHoverPart::OutHandle:
    case CanvasVectorHoverPart::CornerHandle:
        viewport()->setCursor(Qt::PointingHandCursor);
        return;
    case CanvasVectorHoverPart::Segment:
        viewport()->setCursor(Qt::CrossCursor);
        return;
    case CanvasVectorHoverPart::None:
        viewport()->setCursor(m_toolCursor);
        return;
    }
}

void ImageCanvas::setVectorNodeMarquee(const QRectF &documentBounds,
                                        const bool visible)
{
    m_vectorNodeMarqueeBounds = documentBounds.normalized();
    m_vectorNodeMarqueeVisible = visible;
    viewport()->update();
}

void ImageCanvas::clearVectorPathOverlay()
{
    m_vectorPathNodes.clear();
    m_vectorPathEndpointMarkers.clear();
    m_vectorPathSelectedNodes.clear();
    m_vectorPathClosed = false;
    m_vectorPathActiveHandleNode = -1;
    m_vectorPathDisplayPath = {};
    m_vectorCornerEditing = false;
    m_vectorPathHover = {};
    m_vectorNodeMarqueeBounds = {};
    m_vectorNodeMarqueeVisible = false;
    updateVectorPathCursor();
    viewport()->update();
}

void ImageCanvas::setTextEditingActive(const bool active)
{
    m_textEditingActive = active;
}

bool ImageCanvas::textEditingActive() const
{
    return m_textEditingActive;
}

void ImageCanvas::setLeftDragPans(const bool enabled)
{
    m_leftDragPans = enabled;
}

void ImageCanvas::setColourSamplingEnabled(const bool enabled)
{
    m_colourSamplingEnabled = enabled;
    if (!enabled) {
        m_samplingColour = false;
    }
}

void ImageCanvas::setPaintMode(const CanvasPaintMode mode)
{
    if (m_paintMode == mode) {
        return;
    }
    m_paintMode = mode;
    if (mode == CanvasPaintMode::None && m_painting) {
        m_painting = false;
        m_paintPositionValid = false;
        emit paintStrokeFinished();
    }
    viewport()->update();
}

CanvasPaintMode ImageCanvas::paintMode() const
{
    return m_paintMode;
}

void ImageCanvas::cancelPaintGesture()
{
    if (!m_painting) {
        return;
    }
    m_painting = false;
    m_paintPositionValid = false;
    viewport()->update();
}

void ImageCanvas::setBrushDiameter(const double documentPixels)
{
    const double clamped = std::clamp(documentPixels, 1.0, 2000.0);
    if (qFuzzyCompare(m_brushDiameter, clamped)) {
        return;
    }
    m_brushDiameter = clamped;
    viewport()->update();
}

void ImageCanvas::setCloneSourceMarker(const QPointF &documentPosition,
                                       const bool visible)
{
    m_cloneSourceDocumentPosition = documentPosition;
    m_cloneSourceMarkerVisible = visible;
    viewport()->update();
}

void ImageCanvas::clearCloneSourceMarker()
{
    if (!m_cloneSourceMarkerVisible) {
        return;
    }
    m_cloneSourceMarkerVisible = false;
    viewport()->update();
}

void ImageCanvas::setGradientOverlay(const QPointF &startDocument,
                                     const QPointF &endDocument,
                                     const bool visible)
{
    m_gradientStartDocument = startDocument;
    m_gradientEndDocument = endDocument;
    m_gradientOverlayVisible = visible;
    viewport()->update();
}

void ImageCanvas::clearGradientOverlay()
{
    if (!m_gradientOverlayVisible) return;
    m_gradientOverlayVisible = false;
    viewport()->update();
}

void ImageCanvas::setVignetteOverlay(const CanvasVignetteOverlay &overlay,
                                     const bool visible)
{
    CanvasVignetteOverlay normalised = overlay;
    normalised.size = std::clamp(
        std::isfinite(normalised.size) ? normalised.size : 100.0, 10.0, 400.0);
    normalised.midpoint = std::clamp(
        std::isfinite(normalised.midpoint) ? normalised.midpoint : 50.0, 0.0, 100.0);
    normalised.roundness = std::clamp(
        std::isfinite(normalised.roundness) ? normalised.roundness : 0.0, -100.0, 100.0);
    normalised.feather = std::clamp(
        std::isfinite(normalised.feather) ? normalised.feather : 50.0, 0.0, 100.0);
    normalised.centreX = std::clamp(
        std::isfinite(normalised.centreX) ? normalised.centreX : 0.0, -100.0, 100.0);
    normalised.centreY = std::clamp(
        std::isfinite(normalised.centreY) ? normalised.centreY : 0.0, -100.0, 100.0);
    normalised.rotation = std::clamp(
        std::isfinite(normalised.rotation) ? normalised.rotation : 0.0, -180.0, 180.0);

    if (m_vignetteOverlay == normalised && m_vignetteOverlayVisible == visible) {
        return;
    }
    m_vignetteOverlay = normalised;
    m_vignetteOverlayVisible = visible;
    if (!visible) {
        m_vignetteDragging = false;
        m_vignetteDragMode = VignetteDragMode::None;
    }
    viewport()->update();
}

void ImageCanvas::clearVignetteOverlay()
{
    if (!m_vignetteOverlayVisible && !m_vignetteDragging) return;
    m_vignetteOverlayVisible = false;
    m_vignetteDragging = false;
    m_vignetteDragMode = VignetteDragMode::None;
    viewport()->update();
}

bool ImageCanvas::vignetteOverlayVisible() const
{
    return m_vignetteOverlayVisible;
}

void ImageCanvas::setVignetteOverlayCallbacks(
    std::function<void()> interactionStarted,
    std::function<void(double, double, double, double, double)> changed,
    std::function<void()> interactionFinished)
{
    m_vignetteOverlayInteractionStarted = std::move(interactionStarted);
    m_vignetteOverlayChanged = std::move(changed);
    m_vignetteOverlayInteractionFinished = std::move(interactionFinished);
}

void ImageCanvas::beginLiveCompositePreview()
{
    if (!hasImage()) {
        return;
    }
    m_liveStrokeImage = m_image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_liveStrokeImage.setColorSpace(m_image.colorSpace());
    m_displayLiveStrokeImage = displayManagedCopy(m_liveStrokeImage);
}

void ImageCanvas::setLiveCompositePreviewImage(const QImage &renderedImage)
{
    if (!hasImage() || renderedImage.isNull()) {
        clearLiveStrokePreview();
        return;
    }
    m_liveStrokeImage = renderedImage.convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    m_liveStrokeImage.setColorSpace(renderedImage.colorSpace().isValid()
                                        ? renderedImage.colorSpace()
                                        : m_image.colorSpace());
    m_displayLiveStrokeImage = displayManagedCopy(m_liveStrokeImage);
    viewport()->update();
}

void ImageCanvas::updateLiveCompositeRegion(const QRect &previewRegion,
                                            const QImage &renderedRegion)
{
    if (m_liveStrokeImage.isNull() || renderedRegion.isNull()) {
        return;
    }
    const QRect region = previewRegion.intersected(m_liveStrokeImage.rect());
    if (region.isEmpty()) {
        return;
    }
    QPainter painter(&m_liveStrokeImage);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(region.topLeft(), renderedRegion);
    painter.end();
    if (m_displayLiveStrokeImage.isNull()
        || m_displayLiveStrokeImage.size() != m_liveStrokeImage.size()) {
        m_displayLiveStrokeImage = displayManagedCopy(m_liveStrokeImage);
    } else {
        const QImage displayRegion = displayManagedCopy(renderedRegion);
        QPainter displayPainter(&m_displayLiveStrokeImage);
        displayPainter.setCompositionMode(QPainter::CompositionMode_Source);
        displayPainter.drawImage(region.topLeft(), displayRegion);
    }
    viewport()->update(viewportRectForPreviewRegion(region));
}

void ImageCanvas::clearLiveStrokePreview()
{
    if (m_liveStrokeImage.isNull()) {
        return;
    }
    m_liveStrokeImage = {};
    m_displayLiveStrokeImage = {};
    viewport()->update();
}

void ImageCanvas::commitLiveCompositePreview()
{
    if (m_liveStrokeImage.isNull()) {
        return;
    }

    // Normal paint previews match the authoritative preview extent and can be
    // promoted immediately. Performance-bounded tools such as Gradient may
    // publish a smaller screen-resolution image while dragging; never replace
    // the authoritative backing image with that reduced transient surface.
    if (m_liveStrokeImage.size() == m_image.size()) {
        m_image = m_liveStrokeImage;
        m_displayImage = m_displayLiveStrokeImage.isNull()
            ? displayManagedCopy(m_image) : m_displayLiveStrokeImage;
        m_presentationTiles.clear();
    }
    m_liveStrokeImage = {};
    m_displayLiveStrokeImage = {};
    viewport()->update();
}

QImage ImageCanvas::colouriseMaskOverlay(const QImage &maskImage)
{
    if (maskImage.isNull()) {
        return {};
    }

    const QImage grey = maskImage.convertToFormat(QImage::Format_Grayscale8);
    QImage overlay(grey.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < grey.height(); ++y) {
        const uchar *src = grey.constScanLine(y);
        uchar *dst = overlay.scanLine(y);
        for (int x = 0; x < grey.width(); ++x) {
            const int offset = x * 4;
            dst[offset] = 255;
            dst[offset + 1] = 0;
            dst[offset + 2] = 0;
            dst[offset + 3] = static_cast<uchar>((static_cast<int>(src[x]) * 128 + 127) / 255);
        }
    }
    return overlay;
}

void ImageCanvas::setMaskOverlay(const QImage &maskImage)
{
    if (!hasImage() || maskImage.isNull()) {
        clearMaskOverlay();
        return;
    }

    QImage prepared = maskImage;
    if (prepared.size() != m_image.size()) {
        prepared = prepared.scaled(m_image.size(),
                                   Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    m_maskOverlayImage = colouriseMaskOverlay(prepared);
    viewport()->update();
}

void ImageCanvas::setMaskOverlayPreviewImage(const QImage &maskImage)
{
    if (!hasImage() || maskImage.isNull()) {
        clearMaskOverlay();
        return;
    }
    m_maskOverlayImage = colouriseMaskOverlay(maskImage);
    viewport()->update();
}

void ImageCanvas::updateMaskOverlayRegion(const QRect &previewRegion,
                                          const QImage &maskRegion)
{
    if (m_maskOverlayImage.isNull() || maskRegion.isNull()) {
        return;
    }
    const QRect region = previewRegion.intersected(m_maskOverlayImage.rect());
    if (region.isEmpty()) {
        return;
    }

    QImage prepared = maskRegion;
    if (prepared.size() != region.size()) {
        prepared = prepared.scaled(region.size(),
                                   Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    const QImage overlayRegion = colouriseMaskOverlay(prepared);
    QPainter painter(&m_maskOverlayImage);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(region.topLeft(), overlayRegion);
    painter.end();
    viewport()->update(viewportRectForPreviewRegion(region));
}

void ImageCanvas::clearMaskOverlay()
{
    if (m_maskOverlayImage.isNull()) {
        return;
    }
    m_maskOverlayImage = {};
    viewport()->update();
}

bool ImageCanvas::hasMaskOverlay() const
{
    return !m_maskOverlayImage.isNull();
}

void ImageCanvas::beginTreatmentOverlay(const QSize &previewSize)
{
    if (!hasImage() || previewSize.isEmpty()) {
        clearTreatmentOverlay();
        return;
    }
    m_treatmentOverlayImage = QImage(previewSize, QImage::Format_RGBA8888);
    m_treatmentOverlayImage.fill(Qt::transparent);
    viewport()->update();
}

void ImageCanvas::updateTreatmentOverlayRegion(const QRect &previewRegion,
                                                const QImage &coverageRegion)
{
    if (m_treatmentOverlayImage.isNull() || coverageRegion.isNull()) {
        return;
    }
    const QRect region = previewRegion.intersected(m_treatmentOverlayImage.rect());
    if (region.isEmpty()) {
        return;
    }

    QImage prepared = coverageRegion;
    if (prepared.size() != region.size()) {
        prepared = prepared.scaled(region.size(),
                                   Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    }
    const QImage overlayRegion = colouriseMaskOverlay(prepared);
    QPainter painter(&m_treatmentOverlayImage);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(region.topLeft(), overlayRegion);
    painter.end();
    viewport()->update(viewportRectForPreviewRegion(region));
}

void ImageCanvas::clearTreatmentOverlay()
{
    if (m_treatmentOverlayImage.isNull()) {
        return;
    }
    m_treatmentOverlayImage = {};
    viewport()->update();
}

bool ImageCanvas::hasTreatmentOverlay() const
{
    return !m_treatmentOverlayImage.isNull();
}

QPainterPath ImageCanvas::selectionBoundaryPath(const QImage &coverageImage)
{
    const QImage coverage = coverageImage.convertToFormat(QImage::Format_Grayscale8);
    if (coverage.isNull()) {
        return {};
    }
    const auto selected = [&coverage](const int x, const int y) {
        return x >= 0 && y >= 0 && x < coverage.width() && y < coverage.height()
            && coverage.constScanLine(y)[x] >= 128;
    };

    QPainterPath path;
    // Horizontal runs between differing rows.
    for (int edgeY = 0; edgeY <= coverage.height(); ++edgeY) {
        int runStart = -1;
        for (int x = 0; x < coverage.width(); ++x) {
            const bool differs = selected(x, edgeY - 1) != selected(x, edgeY);
            if (differs && runStart < 0) {
                runStart = x;
            } else if (!differs && runStart >= 0) {
                path.moveTo(runStart, edgeY);
                path.lineTo(x, edgeY);
                runStart = -1;
            }
        }
        if (runStart >= 0) {
            path.moveTo(runStart, edgeY);
            path.lineTo(coverage.width(), edgeY);
        }
    }
    // Vertical runs between differing columns.
    for (int edgeX = 0; edgeX <= coverage.width(); ++edgeX) {
        int runStart = -1;
        for (int y = 0; y < coverage.height(); ++y) {
            const bool differs = selected(edgeX - 1, y) != selected(edgeX, y);
            if (differs && runStart < 0) {
                runStart = y;
            } else if (!differs && runStart >= 0) {
                path.moveTo(edgeX, runStart);
                path.lineTo(edgeX, y);
                runStart = -1;
            }
        }
        if (runStart >= 0) {
            path.moveTo(edgeX, runStart);
            path.lineTo(edgeX, coverage.height());
        }
    }
    return path;
}

void ImageCanvas::setSelectionDisplay(const QImage &coverageImage,
                                      const bool active,
                                      const bool edgesVisible)
{
    m_selectionPreviewActive = false;
    m_selectionPreviewBoundaryPath = {};
    m_selectionPreviewCoverageSize = {};
    m_selectionActive = active;
    m_selectionEdgesVisible = edgesVisible;
    m_selectionCoverageSize = coverageImage.size();
    m_selectionBoundaryPath = active
        ? selectionBoundaryPath(coverageImage) : QPainterPath();
    m_selectionAntsPhase = 0;
    if (m_selectionActive && m_selectionEdgesVisible
        && !m_selectionBoundaryPath.isEmpty()) {
        m_selectionAntsTimer.start();
    } else {
        m_selectionAntsTimer.stop();
    }
    viewport()->update();
}

void ImageCanvas::setSelectionEdgesVisible(const bool visible)
{
    if (m_selectionEdgesVisible == visible) {
        return;
    }
    m_selectionEdgesVisible = visible;
    const QPainterPath &displayPath = m_selectionPreviewActive
        ? m_selectionPreviewBoundaryPath : m_selectionBoundaryPath;
    if (visible && !displayPath.isEmpty()
        && (m_selectionActive || m_selectionPreviewActive)) {
        m_selectionAntsTimer.start();
    } else {
        m_selectionAntsTimer.stop();
    }
    viewport()->update();
}

void ImageCanvas::setSelectionPreviewDisplay(const QImage &coverageImage)
{
    m_selectionPreviewCoverageSize = coverageImage.size();
    m_selectionPreviewBoundaryPath = selectionBoundaryPath(coverageImage);
    m_selectionPreviewActive = true;
    m_selectionAntsPhase = 0;
    if (m_selectionEdgesVisible && !m_selectionPreviewBoundaryPath.isEmpty()) {
        m_selectionAntsTimer.start();
    } else {
        m_selectionAntsTimer.stop();
    }
    viewport()->update();
}

void ImageCanvas::clearSelectionPreviewDisplay()
{
    if (!m_selectionPreviewActive) {
        return;
    }
    m_selectionPreviewActive = false;
    m_selectionPreviewBoundaryPath = {};
    m_selectionPreviewCoverageSize = {};
    if (m_selectionActive && m_selectionEdgesVisible
        && !m_selectionBoundaryPath.isEmpty()) {
        m_selectionAntsTimer.start();
    } else {
        m_selectionAntsTimer.stop();
    }
    viewport()->update();
}

bool ImageCanvas::selectionEdgesVisible() const
{
    return m_selectionEdgesVisible;
}

void ImageCanvas::clearSelectionDisplay()
{
    m_selectionAntsTimer.stop();
    m_selectionBoundaryPath = {};
    m_selectionCoverageSize = {};
    m_selectionPreviewBoundaryPath = {};
    m_selectionPreviewCoverageSize = {};
    m_selectionPreviewActive = false;
    m_selectionAntsPhase = 0;
    m_selectionActive = false;
    viewport()->update();
}

void ImageCanvas::setSelectionMarqueeEnabled(const bool enabled)
{
    if (m_selectionMarqueeEnabled == enabled) {
        return;
    }
    if (!enabled) {
        cancelSelectionMarqueeGesture();
    }
    m_selectionMarqueeEnabled = enabled;
    viewport()->update();
}

void ImageCanvas::setSelectionMarqueeFixedOneToOne(const bool enabled)
{
    m_selectionMarqueeFixedOneToOne = enabled;
    if (m_selectionMarqueeDragging) {
        emit selectionMarqueeChanged(currentSelectionMarqueeBounds());
        viewport()->update();
    }
}

void ImageCanvas::setSelectionMarqueeFromCentre(const bool enabled)
{
    m_selectionMarqueeFromCentre = enabled;
    if (m_selectionMarqueeDragging) {
        emit selectionMarqueeChanged(currentSelectionMarqueeBounds());
        viewport()->update();
    }
}

void ImageCanvas::setSelectionMarqueeEllipse(const bool ellipse)
{
    const CanvasMarqueePreviewMode requestedMode = ellipse
        ? CanvasMarqueePreviewMode::Ellipse
        : CanvasMarqueePreviewMode::Rectangle;
    if (m_selectionMarqueeEllipse == ellipse
        && m_selectionMarqueePreviewMode == requestedMode) {
        return;
    }
    m_selectionMarqueeEllipse = ellipse;
    // Shape creation also uses the marquee renderer. Always restore the
    // raster-selection preview mode here, even when the ellipse flag itself
    // did not change, so a previous Polygon/Star/Rounded Rectangle preview
    // cannot leak into Rectangle Select.
    m_selectionMarqueePreviewMode = requestedMode;
    if (m_selectionMarqueeDragging) {
        emit selectionMarqueeChanged(currentSelectionMarqueeBounds());
        viewport()->update();
    }
}

void ImageCanvas::setSelectionMarqueeCornerRadius(const double radius)
{
    const double safeRadius = std::max(0.0, std::isfinite(radius) ? radius : 0.0);
    if (std::abs(m_selectionMarqueeCornerRadius - safeRadius) < 1.0e-9) {
        return;
    }
    m_selectionMarqueeCornerRadius = safeRadius;
    if (m_selectionMarqueeDragging) {
        viewport()->update();
    }
}


void ImageCanvas::setSelectionMarqueePreviewMode(
    const CanvasMarqueePreviewMode mode,
    const int polygonSides,
    const double starInnerRatio,
    const double rotationDegrees,
    const double arrowHeadLengthRatio,
    const double arrowShaftWidthRatio)
{
    m_selectionMarqueePreviewMode = mode;
    m_selectionMarqueePolygonSides = std::clamp(polygonSides, 3, 64);
    m_selectionMarqueeStarInnerRatio = std::clamp(
        std::isfinite(starInnerRatio) ? starInnerRatio : 0.5, 0.01, 0.99);
    m_selectionMarqueeRotationDegrees = std::isfinite(rotationDegrees)
        ? rotationDegrees : -90.0;
    m_selectionMarqueeArrowHeadLengthRatio = std::clamp(
        std::isfinite(arrowHeadLengthRatio) ? arrowHeadLengthRatio : 0.35,
        0.1, 0.9);
    m_selectionMarqueeArrowShaftWidthRatio = std::clamp(
        std::isfinite(arrowShaftWidthRatio) ? arrowShaftWidthRatio : 0.35,
        0.05, 0.95);
    if (m_selectionMarqueeDragging) viewport()->update();
}

CanvasMarqueePreviewMode ImageCanvas::selectionMarqueePreviewMode() const
{
    return m_selectionMarqueePreviewMode;
}

QLineF ImageCanvas::selectionMarqueeLine() const
{
    return currentSelectionMarqueeLine();
}

void ImageCanvas::setSelectionMarqueeDeselectOnClick(const bool enabled)
{
    m_selectionMarqueeDeselectOnClick = enabled;
}

void ImageCanvas::setSelectionMarqueeFinishOnClick(const bool enabled)
{
    m_selectionMarqueeFinishOnClick = enabled;
}

void ImageCanvas::setSelectionMarqueeClipToImage(const bool enabled)
{
    if (m_selectionMarqueeClipToImage == enabled) return;
    m_selectionMarqueeClipToImage = enabled;
    if (m_selectionMarqueeDragging) viewport()->update();
}

void ImageCanvas::setSelectionMarqueeGeometryModifiersEnabled(const bool enabled)
{
    m_selectionMarqueeGeometryModifiersEnabled = enabled;
}

void ImageCanvas::setSelectionMarqueePixelSnappingEnabled(const bool enabled)
{
    if (m_selectionMarqueePixelSnappingEnabled == enabled) return;
    m_selectionMarqueePixelSnappingEnabled = enabled;
    if (m_selectionMarqueeDragging) {
        emit selectionMarqueeChanged(currentSelectionMarqueeBounds());
        viewport()->update();
    }
}

void ImageCanvas::cancelSelectionMarqueeGesture()
{
    if (!m_selectionMarqueeDragging) {
        return;
    }
    m_selectionMarqueeDragging = false;
    m_selectionMarqueeRepositioning = false;
    emit selectionMarqueeCancelled();
    viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
    viewport()->update();
}

void ImageCanvas::setSelectionLassoMode(const CanvasSelectionLassoMode mode)
{
    if (m_selectionLassoMode == mode) {
        return;
    }
    cancelSelectionLassoGesture();
    m_selectionLassoMode = mode;
    viewport()->update();
}

void ImageCanvas::cancelSelectionLassoGesture()
{
    if (!m_selectionLassoActive) {
        return;
    }
    m_selectionLassoActive = false;
    m_selectionLassoPointerDown = false;
    m_selectionLassoPoints.clear();
    m_selectionLassoViewportPoints.clear();
    m_selectionLassoCurrent = {};
    m_selectionLassoCurrentViewport = {};
    emit selectionLassoCancelled();
    viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
    viewport()->update();
}

void ImageCanvas::setTransformDragEnabled(const bool enabled)
{
    m_transformDragEnabled = enabled;
    if (!enabled && m_transformDragging) {
        m_transformDragging = false;
        emit transformDragFinished(m_transformCurrentTransform);
    }
    if (!enabled) {
        m_transformMode = CanvasTransformMode::None;
        m_transformCurrentTransform.reset();
        clearTransformSnapState();
        clearTransformPreview();
    }
    viewport()->update();
}

void ImageCanvas::setTransformSnappingEnabled(const bool enabled)
{
    if (m_transformSnappingEnabled == enabled) {
        return;
    }
    m_transformSnappingEnabled = enabled;
    if (!enabled) {
        clearTransformSnapState();
    }
    viewport()->update();
}

bool ImageCanvas::transformSnappingEnabled() const
{
    return m_transformSnappingEnabled;
}

void ImageCanvas::setTransformSnapDistance(const double screenPixels)
{
    m_transformSnapDistance = std::clamp(screenPixels, 1.0, 128.0);
}

double ImageCanvas::transformSnapDistance() const
{
    return m_transformSnapDistance;
}

void ImageCanvas::setTransformSnapBounds(const QVector<QRectF> &bounds)
{
    clearTransformSnapState();
    m_transformSnapBounds.clear();
    m_transformSnapTargetPoints.clear();
    m_transformSnapSourcePoints.clear();
    m_transformSnapBounds.reserve(bounds.size());
    for (const QRectF &boundsRect : bounds) {
        const QRectF normalized = boundsRect.normalized();
        if (!normalized.isEmpty()) {
            m_transformSnapBounds.push_back(normalized);
        }
    }
}

void ImageCanvas::setTransformSnapPoints(const QVector<QPointF> &targetPoints,
                                         const QVector<QPointF> &sourcePoints)
{
    clearTransformSnapState();
    const auto safePoint = [](const QPointF &point) {
        return std::isfinite(point.x()) && std::isfinite(point.y())
            && std::abs(point.x()) <= 1.0e9 && std::abs(point.y()) <= 1.0e9;
    };
    m_transformSnapTargetPoints.clear();
    m_transformSnapSourcePoints.clear();
    m_transformSnapTargetPoints.reserve(targetPoints.size());
    m_transformSnapSourcePoints.reserve(sourcePoints.size());
    for (const QPointF &point : targetPoints) {
        if (safePoint(point)) m_transformSnapTargetPoints.push_back(point);
    }
    for (const QPointF &point : sourcePoints) {
        if (safePoint(point)) m_transformSnapSourcePoints.push_back(point);
    }
}

void ImageCanvas::setTransformInteractionMode(
    const CanvasTransformInteractionMode mode)
{
    if (m_transformInteractionMode == mode) {
        return;
    }
    m_transformInteractionMode = mode;
    updateTransformCursor(m_lastMouseViewportPosition);
    viewport()->update();
}

CanvasTransformInteractionMode ImageCanvas::transformInteractionMode() const
{
    return m_transformInteractionMode;
}

void ImageCanvas::setTransformInterpolation(
    const TransformInterpolation interpolation)
{
    if (m_transformInterpolation == interpolation) {
        return;
    }
    m_transformInterpolation = interpolation;
    viewport()->update();
}

TransformInterpolation ImageCanvas::transformInterpolation() const
{
    return m_transformInterpolation;
}

void ImageCanvas::setTransformPivot(const QPointF &documentPosition)
{
    m_transformPivotDocument = documentPosition;
    m_transformPivotValid = true;
    viewport()->update();
}

QPointF ImageCanvas::transformPivot() const
{
    return m_transformPivotValid
        ? m_transformPivotDocument
        : m_transformCurrentTransform.map(m_transformDocumentBounds.center());
}

void ImageCanvas::setTransformSessionTransform(
    const QTransform &documentTransform)
{
    m_transformCurrentTransform = documentTransform;
    viewport()->update();
}

QTransform ImageCanvas::transformSessionTransform() const
{
    return m_transformCurrentTransform;
}

void ImageCanvas::setTransformPendingChanges(const bool pending)
{
    if (m_transformPendingChanges == pending) {
        return;
    }
    m_transformPendingChanges = pending;
    viewport()->update();
}

bool ImageCanvas::transformPendingChanges() const
{
    return m_transformPendingChanges;
}

void ImageCanvas::cancelTransformGesture()
{
    if (!m_transformDragging && !m_transformPivotDragging) {
        return;
    }
    m_transformDragging = false;
    m_transformPivotDragging = false;
    m_transformMode = CanvasTransformMode::None;
    m_transformCurrentTransform = m_transformGestureBaseTransform;
    if (m_transformPivotValid) {
        m_transformPivotDocument = m_transformGestureStartPivotDocument;
    }
    clearTransformSnapState();
    viewport()->update();
}

void ImageCanvas::setTransformSelectionBounds(const QRectF &documentBounds)
{
    m_transformDocumentBounds = documentBounds.normalized();
    if (!m_transformDragging) {
        m_transformCurrentTransform.reset();
        m_transformGestureBaseQuad.clear();
        m_transformPivotDocument = m_transformDocumentBounds.center();
        m_transformPivotValid = !m_transformDocumentBounds.isEmpty();
        clearTransformSnapState();
        m_transformPendingChanges = false;
    }
    viewport()->update();
}

void ImageCanvas::clearTransformSelection()
{
    m_transformDocumentBounds = {};
    m_transformCurrentTransform.reset();
    m_transformForegroundBaseTransform.reset();
    m_transformForegroundDocumentBounds = {};
    m_transformGestureBaseTransform.reset();
    m_transformGestureBaseQuad.clear();
    m_transformDragging = false;
    m_transformPivotDragging = false;
    m_transformMode = CanvasTransformMode::None;
    m_transformPivotValid = false;
    m_transformPendingChanges = false;
    clearTransformSnapState();
    viewport()->update();
}

void ImageCanvas::beginTransformPreview(
    const QImage &background,
    const QImage &foreground,
    const QRectF &documentBounds,
    const QTransform &foregroundBaseTransform,
    const QRectF &foregroundDocumentBounds)
{
    m_transformBackground = background;
    m_transformForeground = foreground;
    m_transformCompositePreview = {};
    m_displayTransformBackground = displayManagedCopy(background);
    m_displayTransformForeground = displayManagedCopy(foreground);
    m_displayTransformCompositePreview = {};
    m_transformForegroundAlreadyTransformed = false;
    m_transformGpuPreviewTimer.invalidate();
    m_transformDocumentBounds = documentBounds.normalized();
    m_transformCurrentTransform.reset();
    m_transformForegroundBaseTransform = foregroundBaseTransform;
    m_transformForegroundDocumentBounds = foregroundDocumentBounds.normalized();
    if (m_transformForegroundDocumentBounds.isEmpty()) {
        m_transformForegroundDocumentBounds = QRectF(
            QPointF(0.0, 0.0), QSizeF(m_documentSize));
    }
    m_transformGestureBaseTransform.reset();
    m_transformGestureBaseQuad.clear();
    m_transformPivotDocument = m_transformDocumentBounds.center();
    m_transformPivotValid = !m_transformDocumentBounds.isEmpty();
    m_transformPendingChanges = false;
    clearTransformSnapState();
    m_transformPreviewActive = !background.isNull() && !foreground.isNull();
    viewport()->update();
}

void ImageCanvas::setTransformPreviewForeground(
    const QImage &foreground,
    const bool alreadyTransformed,
    const QRectF &foregroundDocumentBounds)
{
    if (!m_transformPreviewActive || foreground.isNull()) {
        return;
    }

    QRectF resolvedBounds = foregroundDocumentBounds.normalized();
    if (resolvedBounds.isEmpty()) {
        if (foreground.size() == m_transformForeground.size()
            && !m_transformForegroundDocumentBounds.isEmpty()) {
            resolvedBounds = m_transformForegroundDocumentBounds;
        } else if (foreground.size() == m_transformBackground.size()) {
            resolvedBounds = QRectF(QPointF(0.0, 0.0),
                                    QSizeF(m_documentSize));
        } else {
            return;
        }
    }

    m_transformForeground = foreground;
    m_transformForegroundDocumentBounds = resolvedBounds;
    m_displayTransformForeground = displayManagedCopy(foreground);
    m_transformForegroundAlreadyTransformed = alreadyTransformed;
    m_transformCompositePreview = {};
    m_displayTransformCompositePreview = {};
    m_transformGpuPreviewTimer.invalidate();
    viewport()->update();
}

void ImageCanvas::updateTransformPreview(const QTransform &documentTransform)
{
    m_transformCurrentTransform = documentTransform;
    m_transformCompositePreview = {};
    m_displayTransformCompositePreview = {};
    const QTransform previewDocumentTransform =
        m_transformForegroundBaseTransform * documentTransform;
    const QRectF fullDocumentBounds(QPointF(0.0, 0.0),
                                    QSizeF(m_documentSize));
    const QRectF foregroundBounds =
        m_transformForegroundDocumentBounds.normalized();
    const bool foregroundCoversDocument =
        std::abs(foregroundBounds.left() - fullDocumentBounds.left()) <= 1.0e-6
        && std::abs(foregroundBounds.top() - fullDocumentBounds.top()) <= 1.0e-6
        && std::abs(foregroundBounds.right() - fullDocumentBounds.right()) <= 1.0e-6
        && std::abs(foregroundBounds.bottom() - fullDocumentBounds.bottom()) <= 1.0e-6;
    const bool gpuCadenceReady = !m_transformGpuPreviewTimer.isValid()
        || m_transformGpuPreviewTimer.elapsed() >= 24;
    if (m_transformPreviewActive
        && !m_transformForegroundAlreadyTransformed
        && foregroundCoversDocument
        && gpuCadenceReady
        && previewDocumentTransform.type() >= QTransform::TxShear
        && !m_transformBackground.isNull()
        && m_transformBackground.size() == m_transformForeground.size()
        && static_cast<qint64>(m_transformBackground.width())
               * m_transformBackground.height() <= 16000000) {
        if (m_transformGpuPreviewTimer.isValid()) {
            m_transformGpuPreviewTimer.restart();
        } else {
            m_transformGpuPreviewTimer.start();
        }
        const double documentWidth = std::max(1, m_documentSize.width());
        const double documentHeight = std::max(1, m_documentSize.height());
        const double previewWidth = std::max(1, m_transformBackground.width());
        const double previewHeight = std::max(1, m_transformBackground.height());
        const QTransform previewToDocument = QTransform::fromScale(
            documentWidth / previewWidth, documentHeight / previewHeight);
        const QTransform documentToPreview = QTransform::fromScale(
            previewWidth / documentWidth, previewHeight / documentHeight);
        const QTransform previewTransform = previewToDocument
            * previewDocumentTransform * documentToPreview;
        m_transformCompositePreview =
            RenderBackend::instance().transformPreviewComposite(
                m_transformBackground, m_transformForeground, previewTransform);
        m_displayTransformCompositePreview =
            displayManagedCopy(m_transformCompositePreview);
    }
    viewport()->update();
}

void ImageCanvas::commitTransformPreview()
{
    if (!m_transformPreviewActive || m_transformBackground.isNull()
        || m_transformForeground.isNull() || m_image.isNull()) {
        clearTransformPreview();
        return;
    }

    if (!m_transformCompositePreview.isNull()) {
        m_image = m_transformCompositePreview.convertToFormat(
            QImage::Format_ARGB32_Premultiplied);
        m_displayImage = displayManagedCopy(m_image);
        m_presentationTiles.clear();
        clearTransformPreview();
        return;
    }

    QImage committed = m_transformBackground.convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    committed.setColorSpace(m_transformBackground.colorSpace());

    const double documentWidth = std::max(1, m_documentSize.width());
    const double documentHeight = std::max(1, m_documentSize.height());
    const double previewWidth = std::max(1, committed.width());
    const double previewHeight = std::max(1, committed.height());
    const QRectF foregroundBounds =
        m_transformForegroundDocumentBounds.isEmpty()
        ? QRectF(QPointF(0.0, 0.0), QSizeF(m_documentSize))
        : m_transformForegroundDocumentBounds.normalized();
    const QRectF foregroundPreviewBounds(
        foregroundBounds.left() * previewWidth / documentWidth,
        foregroundBounds.top() * previewHeight / documentHeight,
        foregroundBounds.width() * previewWidth / documentWidth,
        foregroundBounds.height() * previewHeight / documentHeight);

    QPainter painter(&committed);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(
        QPainter::SmoothPixmapTransform,
        m_transformInterpolation != TransformInterpolation::NearestNeighbour);
    if (!m_transformForegroundAlreadyTransformed) {
        const QTransform previewToDocument = QTransform::fromScale(
            documentWidth / previewWidth, documentHeight / previewHeight);
        const QTransform documentToPreview = QTransform::fromScale(
            previewWidth / documentWidth, previewHeight / documentHeight);
        painter.setTransform(previewToDocument
                             * (m_transformForegroundBaseTransform
                                * m_transformCurrentTransform)
                             * documentToPreview);
    }
    painter.drawImage(foregroundPreviewBounds, m_transformForeground);
    painter.end();

    m_image = std::move(committed);
    m_displayImage = displayManagedCopy(m_image);
    m_presentationTiles.clear();
    clearTransformPreview();
}

void ImageCanvas::clearTransformPreview()
{
    m_transformPreviewActive = false;
    m_transformBackground = {};
    m_transformForeground = {};
    m_transformCompositePreview = {};
    m_displayTransformBackground = {};
    m_displayTransformForeground = {};
    m_displayTransformCompositePreview = {};
    m_transformForegroundAlreadyTransformed = false;
    m_transformGpuPreviewTimer.invalidate();
    m_transformCurrentTransform.reset();
    m_transformForegroundBaseTransform.reset();
    m_transformForegroundDocumentBounds = {};
    m_transformGestureBaseTransform.reset();
    m_transformGestureBaseQuad.clear();
    m_transformDragging = false;
    m_transformPivotDragging = false;
    m_transformMode = CanvasTransformMode::None;
    m_transformPendingChanges = false;
    clearTransformSnapState();
    viewport()->update();
}


void ImageCanvas::setCropEnabled(const bool enabled)
{
    if (m_cropEnabled == enabled) {
        return;
    }
    m_cropEnabled = enabled;
    resetCropInteraction();
    viewport()->update();
}

bool ImageCanvas::cropEnabled() const
{
    return m_cropEnabled;
}

void ImageCanvas::setCropFrame(const QRectF &documentBounds)
{
    QRectF normalised = documentBounds.normalized();
    if (normalised.width() < 1.0) {
        normalised.setWidth(1.0);
    }
    if (normalised.height() < 1.0) {
        normalised.setHeight(1.0);
    }
    if (m_cropFrame == normalised) {
        return;
    }
    m_cropFrame = normalised;
    viewport()->update();
}

QRectF ImageCanvas::cropFrame() const
{
    return m_cropFrame;
}

void ImageCanvas::setCropConstraint(const CropMode mode,
                                    const double aspectRatio,
                                    const QSize &fixedSize)
{
    m_cropConstraintMode = mode;
    m_cropAspectRatio = aspectRatio > 0.0 ? aspectRatio : 0.0;
    m_cropFixedSize = fixedSize;
    viewport()->update();
}

void ImageCanvas::setCropOverlay(const CropOverlay overlay, const int orientation)
{
    m_cropOverlay = overlay;
    m_cropOverlayOrientation = orientation;
    viewport()->update();
}

void ImageCanvas::setCropDimOpacity(const double opacity)
{
    m_cropDimOpacity = std::clamp(opacity, 0.0, 0.95);
    viewport()->update();
}

void ImageCanvas::setCropSnappingEnabled(const bool enabled)
{
    m_cropSnappingEnabled = enabled;
}

void ImageCanvas::setCropPreviewAngle(const double degrees)
{
    if (qFuzzyCompare(m_cropPreviewAngle + 1.0, degrees + 1.0)) {
        return;
    }
    m_cropPreviewAngle = degrees;
    viewport()->update();
}

void ImageCanvas::setCropStraightenSampling(const bool enabled)
{
    m_cropStraightenSampling = enabled;
    m_cropStraightenDragging = false;
    viewport()->setCursor(enabled ? Qt::CrossCursor : m_toolCursor);
    viewport()->update();
}

void ImageCanvas::setCropSnapBounds(const QVector<QRectF> &bounds)
{
    m_cropSnapBounds = bounds;
}

void ImageCanvas::resetCropInteraction()
{
    m_cropDragging = false;
    m_cropDragMode = CropDragMode::None;
    m_cropCreateRepositioning = false;
    m_cropStraightenDragging = false;
    if (!m_spaceHeld) {
        viewport()->setCursor(m_cropEnabled ? Qt::CrossCursor : m_toolCursor);
    }
}

void ImageCanvas::setRulersVisible(const bool visible)
{
    if (m_rulersVisible == visible && m_horizontalRuler->isVisible() == visible) {
        return;
    }

    m_rulersVisible = visible;
    const int margin = visible ? m_rulerThickness : 0;
    setViewportMargins(margin, margin, 0, 0);
    m_horizontalRuler->setVisible(visible);
    m_verticalRuler->setVisible(visible);
    m_rulerCorner->setVisible(visible);
    updateRulerGeometry();
    if (m_fitMode && hasImage()) {
        fitToView();
    } else {
        updateScrollBars();
        viewport()->update();
    }
}

bool ImageCanvas::rulersVisible() const
{
    return m_rulersVisible;
}

void ImageCanvas::setGuidesVisible(const bool visible)
{
    if (m_guidesVisible == visible) {
        return;
    }
    m_guidesVisible = visible;
    viewport()->update();
    emit guidesVisibilityChanged(visible);
}

bool ImageCanvas::guidesVisible() const
{
    return m_guidesVisible;
}

void ImageCanvas::setGuideSnappingEnabled(const bool enabled)
{
    m_snapGuides = enabled;
}

bool ImageCanvas::guideSnappingEnabled() const
{
    return m_snapGuides;
}

void ImageCanvas::setGuides(const QVector<double> &horizontal,
                            const QVector<double> &vertical)
{
    m_horizontalGuides = horizontal;
    m_verticalGuides = vertical;
    sortAndDeduplicate(m_horizontalGuides);
    sortAndDeduplicate(m_verticalGuides);
    viewport()->update();
}

const QVector<double> &ImageCanvas::horizontalGuides() const
{
    return m_horizontalGuides;
}

const QVector<double> &ImageCanvas::verticalGuides() const
{
    return m_verticalGuides;
}

void ImageCanvas::clearGuides()
{
    if (m_horizontalGuides.isEmpty() && m_verticalGuides.isEmpty()) {
        return;
    }
    m_horizontalGuides.clear();
    m_verticalGuides.clear();
    cancelGuideDrag();
    viewport()->update();
    commitGuidesChanged();
}

void ImageCanvas::setSnapBounds(const QVector<QRectF> &bounds)
{
    m_snapBounds = bounds;
}


QPointF ImageCanvas::vignetteCentreDocument() const
{
    if (!m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return {};
    }
    return QPointF(
        m_documentSize.width() * (0.5 + m_vignetteOverlay.centreX / 200.0),
        m_documentSize.height() * (0.5 + m_vignetteOverlay.centreY / 200.0));
}

QSizeF ImageCanvas::vignetteEffectiveHalfAxes(const double size) const
{
    if (!m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return {};
    }

    const double scale = std::clamp(size, 10.0, 400.0) / 100.0;
    const double halfWidth = std::max(0.5, m_documentSize.width() * 0.5 * scale);
    const double halfHeight = std::max(0.5, m_documentSize.height() * 0.5 * scale);
    const double halfMinimum = std::max(
        0.5, std::min(m_documentSize.width(), m_documentSize.height()) * 0.5 * scale);
    const double circleMix = std::max(0.0, m_vignetteOverlay.roundness) / 100.0;

    // The CPU renderer blends ellipse and circle inverse radii before applying
    // the superellipse distance. Resolve that same blend to effective axes so
    // the canvas guide is a faithful representation of the rendered mask.
    const double inverseWidth = (1.0 - circleMix) / halfWidth
        + circleMix / halfMinimum;
    const double inverseHeight = (1.0 - circleMix) / halfHeight
        + circleMix / halfMinimum;
    return QSizeF(1.0 / std::max(1.0e-9, inverseWidth),
                  1.0 / std::max(1.0e-9, inverseHeight));
}

double ImageCanvas::vignetteStartDistance() const
{
    return std::clamp(m_vignetteOverlay.midpoint, 0.0, 100.0) / 100.0 * 0.9;
}

double ImageCanvas::vignetteEndDistance() const
{
    const double start = vignetteStartDistance();
    const QSizeF axes = vignetteEffectiveHalfAxes(m_vignetteOverlay.size);
    const double halfMinimum = std::max(0.5, std::min(axes.width(), axes.height()));
    const double feather = std::clamp(m_vignetteOverlay.feather, 0.0, 100.0) / 100.0;
    return start + std::max(1.0 / halfMinimum, (1.0 - start) * feather);
}

QPainterPath ImageCanvas::vignettePath(const double distance) const
{
    QPainterPath path;
    if (!m_vignetteOverlayVisible || !hasImage() || distance <= 0.0) {
        return path;
    }

    const QPointF centre = vignetteCentreDocument();
    const QSizeF axes = vignetteEffectiveHalfAxes(m_vignetteOverlay.size);
    if (axes.isEmpty()) return path;

    const double exponent = 2.0
        + std::max(0.0, -m_vignetteOverlay.roundness) * 0.06;
    const double power = 2.0 / exponent;
    const double rotationRadians = m_vignetteOverlay.rotation
        * std::numbers::pi / 180.0;
    const double cosRotation = std::cos(rotationRadians);
    const double sinRotation = std::sin(rotationRadians);
    constexpr int SegmentCount = 160;

    for (int index = 0; index <= SegmentCount; ++index) {
        const double angle = 2.0 * std::numbers::pi
            * static_cast<double>(index) / SegmentCount;
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        const double localX = axes.width() * distance
            * std::copysign(std::pow(std::abs(cosine), power), cosine);
        const double localY = axes.height() * distance
            * std::copysign(std::pow(std::abs(sine), power), sine);
        const QPointF documentPoint(
            centre.x() + cosRotation * localX - sinRotation * localY,
            centre.y() + sinRotation * localX + cosRotation * localY);
        if (index == 0) path.moveTo(documentPoint);
        else path.lineTo(documentPoint);
    }
    path.closeSubpath();
    return path;
}

QPointF ImageCanvas::vignetteHandleDocumentPosition(
    const VignetteDragMode mode) const
{
    const QPointF centre = vignetteCentreDocument();
    if (mode == VignetteDragMode::Centre || mode == VignetteDragMode::None) {
        return centre;
    }

    const QSizeF axes = vignetteEffectiveHalfAxes(m_vignetteOverlay.size);
    QPointF local;
    if (mode == VignetteDragMode::Size) {
        local = QPointF(axes.width(), 0.0);
    } else if (mode == VignetteDragMode::Midpoint) {
        local = QPointF(0.0, axes.height() * vignetteStartDistance());
    } else if (mode == VignetteDragMode::Rotation) {
        const double scale = std::max(0.01, documentScale(Qt::Horizontal));
        local = QPointF(0.0, -axes.height() - 28.0 / scale);
    }

    const double radians = m_vignetteOverlay.rotation
        * std::numbers::pi / 180.0;
    return QPointF(centre.x() + std::cos(radians) * local.x()
                       - std::sin(radians) * local.y(),
                   centre.y() + std::sin(radians) * local.x()
                       + std::cos(radians) * local.y());
}

ImageCanvas::VignetteDragMode ImageCanvas::vignetteModeAt(
    const QPointF &viewportPosition) const
{
    if (!m_vignetteOverlayVisible || !hasImage()) {
        return VignetteDragMode::None;
    }
    constexpr double Tolerance = 10.0;
    const std::array<VignetteDragMode, 4> modes {
        VignetteDragMode::Centre,
        VignetteDragMode::Size,
        VignetteDragMode::Midpoint,
        VignetteDragMode::Rotation
    };
    for (const VignetteDragMode mode : modes) {
        if (QLineF(viewportPosition,
                   mapDocumentToViewport(vignetteHandleDocumentPosition(mode))).length()
            <= Tolerance) {
            return mode;
        }
    }
    return VignetteDragMode::None;
}

double ImageCanvas::vignetteDistanceAt(const QPointF &documentPosition,
                                       const double size) const
{
    const QPointF delta = documentPosition - vignetteCentreDocument();
    const double radians = -m_vignetteOverlay.rotation
        * std::numbers::pi / 180.0;
    const QPointF local(std::cos(radians) * delta.x()
                            - std::sin(radians) * delta.y(),
                        std::sin(radians) * delta.x()
                            + std::cos(radians) * delta.y());
    const QSizeF axes = vignetteEffectiveHalfAxes(size);
    if (axes.isEmpty()) return 0.0;

    const double exponent = 2.0
        + std::max(0.0, -m_vignetteOverlay.roundness) * 0.06;
    const double x = std::abs(local.x()) / std::max(0.5, axes.width());
    const double y = std::abs(local.y()) / std::max(0.5, axes.height());
    return std::pow(std::pow(x, exponent) + std::pow(y, exponent),
                    1.0 / exponent);
}

void ImageCanvas::updateVignetteFromPointer(const QPointF &documentPosition)
{
    if (!m_vignetteDragging || m_vignetteDragMode == VignetteDragMode::None
        || !m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return;
    }

    if (m_vignetteDragMode == VignetteDragMode::Centre) {
        m_vignetteOverlay.centreX = std::clamp(
            (documentPosition.x() / m_documentSize.width() - 0.5) * 200.0,
            -100.0, 100.0);
        m_vignetteOverlay.centreY = std::clamp(
            (documentPosition.y() / m_documentSize.height() - 0.5) * 200.0,
            -100.0, 100.0);
    } else if (m_vignetteDragMode == VignetteDragMode::Size) {
        m_vignetteOverlay.size = std::clamp(
            vignetteDistanceAt(documentPosition, 100.0) * 100.0,
            10.0, 400.0);
    } else if (m_vignetteDragMode == VignetteDragMode::Midpoint) {
        m_vignetteOverlay.midpoint = std::clamp(
            vignetteDistanceAt(documentPosition, m_vignetteOverlay.size)
                / 0.9 * 100.0,
            0.0, 100.0);
    } else if (m_vignetteDragMode == VignetteDragMode::Rotation) {
        const QPointF delta = documentPosition - vignetteCentreDocument();
        double rotation = std::atan2(delta.y(), delta.x())
            * 180.0 / std::numbers::pi + 90.0;
        while (rotation > 180.0) rotation -= 360.0;
        while (rotation <= -180.0) rotation += 360.0;
        m_vignetteOverlay.rotation = rotation;
    }

    if (m_vignetteOverlayChanged) {
        m_vignetteOverlayChanged(m_vignetteOverlay.size,
                                 m_vignetteOverlay.midpoint,
                                 m_vignetteOverlay.centreX,
                                 m_vignetteOverlay.centreY,
                                 m_vignetteOverlay.rotation);
    }
    viewport()->update();
}

void ImageCanvas::updateVignetteCursor(const QPointF &viewportPosition)
{
    switch (m_vignetteDragging ? m_vignetteDragMode
                               : vignetteModeAt(viewportPosition)) {
    case VignetteDragMode::Centre:
        viewport()->setCursor(Qt::SizeAllCursor);
        break;
    case VignetteDragMode::Size:
        viewport()->setCursor(Qt::SizeHorCursor);
        break;
    case VignetteDragMode::Midpoint:
        viewport()->setCursor(Qt::SizeVerCursor);
        break;
    case VignetteDragMode::Rotation:
        viewport()->setCursor(Qt::CrossCursor);
        break;
    case VignetteDragMode::None:
        viewport()->setCursor(m_toolCursor);
        break;
    }
}

void ImageCanvas::paintVignetteOverlay(QPainter &painter) const
{
    if (!m_vignetteOverlayVisible || !hasImage()) return;

    const QTransform mapping = documentToViewportTransform();
    const QColor accent(QStringLiteral("#61dafb"));
    const QColor sizeColour(QStringLiteral("#ffd166"));
    const QColor midpointColour(QStringLiteral("#ff9f43"));
    const QColor centreColour(QStringLiteral("#f4f7ff"));

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);

    const auto drawGuide = [&painter, &mapping](const QPainterPath &documentPath,
                                                const QColor &colour,
                                                const Qt::PenStyle style,
                                                const double width) {
        if (documentPath.isEmpty()) return;
        const QPainterPath path = mapping.map(documentPath);
        QPen underlay(QColor(0, 0, 0, 210), width + 2.0, style);
        underlay.setDashOffset(0.0);
        painter.setPen(underlay);
        painter.drawPath(path);
        QPen foreground(colour, width, style);
        foreground.setDashOffset(0.0);
        painter.setPen(foreground);
        painter.drawPath(path);
    };

    drawGuide(vignettePath(vignetteEndDistance()),
              QColor(255, 255, 255, 190), Qt::DashLine, 1.0);
    drawGuide(vignettePath(1.0), accent, Qt::SolidLine, 1.25);
    drawGuide(vignettePath(vignetteStartDistance()),
              midpointColour, Qt::DashLine, 1.0);

    const QPointF centre = mapping.map(
        vignetteHandleDocumentPosition(VignetteDragMode::Centre));
    const QPointF size = mapping.map(
        vignetteHandleDocumentPosition(VignetteDragMode::Size));
    const QPointF midpoint = mapping.map(
        vignetteHandleDocumentPosition(VignetteDragMode::Midpoint));
    const QPointF rotation = mapping.map(
        vignetteHandleDocumentPosition(VignetteDragMode::Rotation));

    painter.setPen(QPen(QColor(0, 0, 0, 220), 3.0));
    painter.drawLine(centre, rotation);
    painter.setPen(QPen(QColor(255, 255, 255, 215), 1.0, Qt::DashLine));
    painter.drawLine(centre, rotation);

    const auto drawHandle = [&painter](const QPointF &point,
                                       const QColor &colour,
                                       const double radius) {
        painter.setBrush(QColor(0, 0, 0, 225));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(point, radius + 2.0, radius + 2.0);
        painter.setBrush(colour);
        painter.drawEllipse(point, radius, radius);
        painter.setBrush(Qt::NoBrush);
    };
    drawHandle(centre, centreColour, 4.5);
    drawHandle(size, sizeColour, 5.0);
    drawHandle(midpoint, midpointColour, 4.5);
    drawHandle(rotation, accent, 4.5);

    painter.setPen(QPen(QColor(0, 0, 0, 220), 3.0));
    painter.drawLine(centre - QPointF(8.0, 0.0), centre + QPointF(8.0, 0.0));
    painter.drawLine(centre - QPointF(0.0, 8.0), centre + QPointF(0.0, 8.0));
    painter.setPen(QPen(centreColour, 1.0));
    painter.drawLine(centre - QPointF(8.0, 0.0), centre + QPointF(8.0, 0.0));
    painter.drawLine(centre - QPointF(0.0, 8.0), centre + QPointF(0.0, 8.0));
    painter.restore();
}

void ImageCanvas::paintVectorPathOverlay(QPainter &painter) const
{
    if (!m_vectorPathEditingEnabled
        || (m_vectorPathNodes.isEmpty() && m_vectorPathEndpointMarkers.isEmpty())) {
        return;
    }
    const QTransform mapping = documentToViewportTransform();
    const QColor accent(QStringLiteral("#ff9f43"));
    const QColor selected(QStringLiteral("#ffd166"));
    const QColor handleColour(QStringLiteral("#61dafb"));
    const QColor hoverColour(QStringLiteral("#f4f7ff"));
    const QColor hoverOutline(QStringLiteral("#22d3ee"));

    QPainterPath path;
    if (!m_vectorPathNodes.isEmpty() && !m_vectorPathDisplayPath.isEmpty()) {
        path = mapping.map(m_vectorPathDisplayPath);
    } else if (!m_vectorPathNodes.isEmpty()) {
        path.moveTo(mapping.map(m_vectorPathNodes.constFirst().anchor));
        const int segmentCount = m_vectorPathNodes.size() < 2 ? 0
            : (m_vectorPathClosed ? m_vectorPathNodes.size()
                                  : m_vectorPathNodes.size() - 1);
        for (int segment = 0; segment < segmentCount; ++segment) {
            const int next = (segment + 1) % m_vectorPathNodes.size();
            const CanvasVectorPathNode &left = m_vectorPathNodes.at(segment);
            const CanvasVectorPathNode &right = m_vectorPathNodes.at(next);
            const QPointF c1 = mapping.map(left.outHandleActive
                                               ? left.outHandle : left.anchor);
            const QPointF c2 = mapping.map(right.inHandleActive
                                               ? right.inHandle : right.anchor);
            const QPointF end = mapping.map(right.anchor);
            if ((!left.outHandleActive || left.outHandle == left.anchor)
                && (!right.inHandleActive || right.inHandle == right.anchor)) {
                path.lineTo(end);
            } else {
                path.cubicTo(c1, c2, end);
            }
        }
        if (m_vectorPathClosed && m_vectorPathNodes.size() >= 2) path.closeSubpath();
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!path.isEmpty()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 190), 3.0));
        painter.drawPath(path);
        painter.setPen(QPen(accent, 1.5));
        painter.drawPath(path);
    }

    if (m_vectorPathHover.part == CanvasVectorHoverPart::Segment
        && !m_vectorPathHover.segmentPath.isEmpty()) {
        const QPainterPath hoveredPath = mapping.map(m_vectorPathHover.segmentPath);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 220), 5.5));
        painter.drawPath(hoveredPath);
        painter.setPen(QPen(hoverOutline, 3.0));
        painter.drawPath(hoveredPath);
        painter.setPen(QPen(hoverColour, 1.0));
        painter.drawPath(hoveredPath);
    }

    const auto partHovered = [this](const CanvasVectorHoverPart part,
                                    const int nodeIndex) {
        return m_vectorPathHover.part == part
            && m_vectorPathHover.nodeIndex == nodeIndex;
    };

    for (int index = 0; index < m_vectorPathNodes.size(); ++index) {
        const CanvasVectorPathNode &node = m_vectorPathNodes.at(index);
        const QPointF anchor = mapping.map(node.anchor);
        const bool selectedNode = m_vectorPathSelectedNodes.contains(index);
        const bool anchorHovered = partHovered(CanvasVectorHoverPart::Anchor, index);
        const bool inHovered = partHovered(CanvasVectorHoverPart::InHandle, index);
        const bool outHovered = partHovered(CanvasVectorHoverPart::OutHandle, index);
        const bool cornerHovered = partHovered(CanvasVectorHoverPart::CornerHandle, index);
        const bool showHandles = selectedNode
            || index == m_vectorPathActiveHandleNode
            || inHovered || outHovered;
        if (showHandles) {
            painter.setPen(QPen(QColor(0, 0, 0, 180), 3.0));
            if (node.inHandleActive) painter.drawLine(anchor, mapping.map(node.inHandle));
            if (node.outHandleActive) painter.drawLine(anchor, mapping.map(node.outHandle));
            painter.setPen(QPen(handleColour, 1.0));
            if (node.inHandleActive) painter.drawLine(anchor, mapping.map(node.inHandle));
            if (node.outHandleActive) painter.drawLine(anchor, mapping.map(node.outHandle));

            const auto drawBezierHandle = [&](const QPointF &point,
                                              const bool hovered) {
                const double radius = hovered ? 6.0 : 4.0;
                painter.setPen(QPen(QColor(0, 0, 0, 220), hovered ? 2.5 : 1.0));
                painter.setBrush(hovered ? hoverColour : handleColour);
                painter.drawEllipse(point, radius, radius);
                if (hovered) {
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(QPen(hoverOutline, 2.0));
                    painter.drawEllipse(point, radius - 1.0, radius - 1.0);
                }
            };
            if (node.inHandleActive) {
                drawBezierHandle(mapping.map(node.inHandle), inHovered);
            }
            if (node.outHandleActive) {
                drawBezierHandle(mapping.map(node.outHandle), outHovered);
            }
        }
        if (m_vectorCornerEditing && node.cornerHandleActive) {
            const QPointF handle = mapping.map(node.cornerHandle);
            const double radius = QLineF(anchor, handle).length();
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(97, 218, 251, 155), 1.0, Qt::DashLine));
            if (radius > 2.0 && (selectedNode
                                 || index == m_vectorPathActiveHandleNode
                                 || cornerHovered)) {
                painter.drawEllipse(anchor, radius, radius);
            }
            painter.setPen(QPen(cornerHovered ? hoverOutline : handleColour,
                                cornerHovered ? 2.5 : 1.25));
            painter.drawLine(anchor, handle);
            const double handleRadius = cornerHovered ? 6.5 : 4.5;
            painter.setBrush(cornerHovered ? hoverColour
                                           : selectedNode ? selected : handleColour);
            painter.setPen(QPen(QColor(0, 0, 0, 220), cornerHovered ? 2.5 : 1.0));
            painter.drawEllipse(handle, handleRadius, handleRadius);
            if (cornerHovered) {
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(hoverOutline, 2.0));
                painter.drawEllipse(handle, handleRadius - 1.0,
                                    handleRadius - 1.0);
            }
        }

        const double halfSize = anchorHovered ? 6.0 : 4.5;
        const QRectF anchorRect(anchor - QPointF(halfSize, halfSize),
                                QSizeF(halfSize * 2.0, halfSize * 2.0));
        painter.setBrush(selectedNode ? selected
                                      : anchorHovered ? hoverColour
                                                      : QColor(Qt::white));
        painter.setPen(QPen(anchorHovered ? hoverOutline : QColor(0, 0, 0, 220),
                            anchorHovered ? 2.5 : 1.0));
        painter.drawRect(anchorRect);
        if (selectedNode && anchorHovered) {
            const QRectF inner(anchor - QPointF(3.0, 3.0), QSizeF(6.0, 6.0));
            painter.setPen(QPen(hoverOutline, 1.5));
            painter.setBrush(selected);
            painter.drawRect(inner);
        }
    }

    for (const CanvasVectorEndpointMarker &marker
         : m_vectorPathEndpointMarkers) {
        const QPointF position = mapping.map(marker.position);
        const double radius = marker.hovered ? 7.0 : 5.0;
        QColor fill;
        QColor outline;
        switch (marker.role) {
        case CanvasVectorEndpointRole::Continue:
            fill = QColor(255, 255, 255, 245);
            outline = accent;
            break;
        case CanvasVectorEndpointRole::Close:
            fill = QColor(QStringLiteral("#79d98c"));
            outline = QColor(QStringLiteral("#1f6f35"));
            break;
        case CanvasVectorEndpointRole::Join:
            fill = handleColour;
            outline = QColor(QStringLiteral("#155f78"));
            break;
        case CanvasVectorEndpointRole::Active:
            fill = accent;
            outline = QColor(QStringLiteral("#7a4300"));
            break;
        }
        painter.setPen(QPen(QColor(0, 0, 0, 210), marker.hovered ? 3.0 : 2.0));
        painter.setBrush(marker.hovered ? hoverColour : fill);
        painter.drawEllipse(position, radius + 1.0, radius + 1.0);
        painter.setPen(QPen(marker.hovered ? hoverOutline : outline,
                            marker.hovered ? 2.5 : 1.25));
        painter.drawEllipse(position, radius, radius);
        if (marker.role == CanvasVectorEndpointRole::Continue) {
            painter.drawLine(position + QPointF(-2.5, 0.0),
                             position + QPointF(2.5, 0.0));
            painter.drawLine(position + QPointF(0.0, -2.5),
                             position + QPointF(0.0, 2.5));
        } else if (marker.role == CanvasVectorEndpointRole::Close) {
            painter.setBrush(outline);
            painter.drawEllipse(position, 1.75, 1.75);
        } else if (marker.role == CanvasVectorEndpointRole::Join) {
            const QPolygonF diamond {
                position + QPointF(0.0, -3.0),
                position + QPointF(3.0, 0.0),
                position + QPointF(0.0, 3.0),
                position + QPointF(-3.0, 0.0)
            };
            painter.setBrush(outline);
            painter.drawPolygon(diamond);
        }
    }
    if (m_vectorNodeMarqueeVisible) {
        const QRectF marquee = mapping.mapRect(m_vectorNodeMarqueeBounds).normalized();
        QPen shadowPen(QColor(0, 0, 0, 220), 3.0);
        shadowPen.setCosmetic(true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(shadowPen);
        painter.drawRect(marquee);
        QPen marqueePen(QColor(255, 255, 255, 245), 1.0, Qt::DashLine);
        marqueePen.setCosmetic(true);
        painter.setPen(marqueePen);
        painter.drawRect(marquee);
    }
    painter.restore();
}

void ImageCanvas::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), themeColour(QStringLiteral("preview_background")));

    if (!hasImage()) {
        painter.setPen(themeColour(QStringLiteral("text_muted")));
        painter.drawText(viewport()->rect(),
                         Qt::AlignCenter,
                         tr("Drop an image here or use File → Open Image"));
        return;
    }

    const QRectF target = imageRect();
    const int checkerSize = 16;
    const QColor checkerA = themeColour(QStringLiteral("checker_light"));
    const QColor checkerB = themeColour(QStringLiteral("checker_dark"));

    // The checkerboard is a screen-space decoration. Iterating over the whole
    // zoomed image made repaint cost grow with zoom: a 4096px image at 3200%
    // created roughly 67 million cells per frame. Restrict all checker work to
    // the exposed viewport while preserving alignment to the image origin.
    const QRect exposed = event->region().boundingRect().intersected(viewport()->rect());
    const QRectF checkerVisible = target.intersected(QRectF(exposed));
    if (!checkerVisible.isEmpty()) {
        painter.save();
        painter.setClipRect(checkerVisible);
        const int firstColumn = static_cast<int>(std::floor(
            (checkerVisible.left() - target.left()) / checkerSize));
        const int lastColumn = static_cast<int>(std::floor(
            (checkerVisible.right() - target.left()) / checkerSize));
        const int firstRow = static_cast<int>(std::floor(
            (checkerVisible.top() - target.top()) / checkerSize));
        const int lastRow = static_cast<int>(std::floor(
            (checkerVisible.bottom() - target.top()) / checkerSize));
        for (int row = firstRow; row <= lastRow; ++row) {
            const double y = target.top() + row * checkerSize;
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const double x = target.left() + column * checkerSize;
                painter.fillRect(QRectF(x, y, checkerSize, checkerSize),
                                 ((column + row) & 1) == 0 ? checkerA : checkerB);
            }
        }
        painter.restore();
    }

    const bool rotateCropPreview = m_cropEnabled
        && std::abs(m_cropPreviewAngle) > 1.0e-6;
    if (rotateCropPreview) {
        painter.save();
        QTransform rotation;
        rotation.translate(target.center().x(), target.center().y());
        rotation.rotate(m_cropPreviewAngle);
        rotation.translate(-target.center().x(), -target.center().y());
        painter.setWorldTransform(rotation, true);
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
    if (m_transformPreviewActive) {
        if (!m_transformCompositePreview.isNull()) {
            painter.drawImage(target, m_displayTransformCompositePreview.isNull()
                                          ? m_transformCompositePreview
                                          : m_displayTransformCompositePreview);
        } else {
            painter.drawImage(target, m_displayTransformBackground.isNull()
                                          ? m_transformBackground
                                          : m_displayTransformBackground);
            const QTransform docToViewport = documentToViewportTransform();
            const QRectF foregroundDocumentBounds =
                m_transformForegroundDocumentBounds.isEmpty()
                ? QRectF(QPointF(0.0, 0.0), QSizeF(m_documentSize))
                : m_transformForegroundDocumentBounds.normalized();
            const QRectF foregroundTarget =
                docToViewport.mapRect(foregroundDocumentBounds);
            if (m_transformForegroundAlreadyTransformed) {
                painter.drawImage(foregroundTarget,
                                  m_displayTransformForeground.isNull()
                                      ? m_transformForeground
                                      : m_displayTransformForeground);
            } else {
                painter.save();
                bool invertible = false;
                const QTransform viewportToDoc = docToViewport.inverted(&invertible);
                if (invertible) {
                    painter.setTransform(viewportToDoc
                                         * (m_transformForegroundBaseTransform
                                            * m_transformCurrentTransform)
                                         * docToViewport);
                }
                painter.setRenderHint(
                    QPainter::SmoothPixmapTransform,
                    m_transformInterpolation != TransformInterpolation::NearestNeighbour
                        || m_zoom < 1.0);
                painter.drawImage(foregroundTarget,
                                  m_displayTransformForeground.isNull()
                                      ? m_transformForeground
                                      : m_displayTransformForeground);
                painter.restore();
            }
        }
    } else if (!m_liveStrokeImage.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              m_liveStrokeImage.size() != m_image.size()
                                  || m_zoom < 1.0);
        painter.drawImage(target, m_displayLiveStrokeImage.isNull()
                                      ? m_liveStrokeImage
                                      : m_displayLiveStrokeImage);
    } else {
        struct VisiblePresentationTile {
            QString key;
            QRectF target;
            QRegion clip;
            int level = 0;
        };

        QList<VisiblePresentationTile> visibleTiles;
        QRegion directCoverage;
        if (!m_presentationTiles.isEmpty()) {
            const double scaleX = target.width() / std::max(1, m_image.width());
            const double scaleY = target.height() / std::max(1, m_image.height());
            const QRect viewportRect = viewport()->rect();
            const QList<QString> keys = m_presentationTiles.keys();
            visibleTiles.reserve(keys.size());
            for (const QString &key : keys) {
                PresentationTile &tile = m_presentationTiles[key];
                const QRect &base = tile.basePreviewRect;
                const QRectF tileTarget(target.left() + base.x() * scaleX,
                                        target.top() + base.y() * scaleY,
                                        base.width() * scaleX,
                                        base.height() * scaleY);
                const QRect targetPixels = tileTarget.toAlignedRect().intersected(viewportRect);
                if (targetPixels.isEmpty()) {
                    continue;
                }

                QRegion tileClip(targetPixels);
                const QRegion coveredBase = m_authoritativePreviewCoverage.intersected(
                    QRegion(base));
                for (const QRect &covered : coveredBase) {
                    const QRectF coveredTarget(target.left() + covered.x() * scaleX,
                                               target.top() + covered.y() * scaleY,
                                               covered.width() * scaleX,
                                               covered.height() * scaleY);
                    tileClip = tileClip.subtracted(QRegion(coveredTarget.toAlignedRect()));
                }
                if (tileClip.isEmpty()) {
                    continue;
                }

                visibleTiles.push_back({key, tileTarget, tileClip, tile.level});
                directCoverage = directCoverage.united(tileClip);
                tile.lastUseSerial = ++m_presentationUseSerial;
            }
        }

        // Preserve evicted and not-yet-published areas from the incremental
        // backing image, but do not repaint it underneath live tile records.
        // This keeps transparent tile pixels correct and avoids processing a
        // full-frame fallback once the visible viewport has direct coverage.
        painter.save();
        painter.setClipRegion(QRegion(viewport()->rect()).subtracted(directCoverage));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
        painter.drawImage(target, m_displayImage.isNull() ? m_image : m_displayImage);
        painter.restore();

        std::sort(visibleTiles.begin(),
                  visibleTiles.end(),
                  [](const VisiblePresentationTile &left,
                     const VisiblePresentationTile &right) {
                      if (left.level != right.level) {
                          return left.level > right.level; // coarse first
                      }
                      return left.key < right.key;
                  });
        for (const VisiblePresentationTile &tile : std::as_const(visibleTiles)) {
            QRegion tileClip = tile.clip;
            for (const VisiblePresentationTile &finer : std::as_const(visibleTiles)) {
                if (finer.level < tile.level) {
                    tileClip = tileClip.subtracted(finer.clip);
                }
            }
            if (tileClip.isEmpty()) {
                continue;
            }
            painter.save();
            painter.setClipRegion(tileClip);
            const auto record = m_presentationTiles.constFind(tile.key);
            if (record == m_presentationTiles.cend()) {
                painter.restore();
                continue;
            }
            painter.setRenderHint(QPainter::SmoothPixmapTransform,
                                  tile.level > 0 || m_zoom < 1.0);
            painter.drawImage(tile.target, record->displayImage.isNull()
                                               ? record->image
                                               : record->displayImage);
            painter.restore();
        }
    }

    if (!m_maskOverlayImage.isNull()) {
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              m_maskOverlayImage.size() != m_image.size()
                                  || m_zoom < 1.0);
        painter.drawImage(target, m_maskOverlayImage);
        painter.restore();
    }

    if (!m_treatmentOverlayImage.isNull()) {
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
        painter.drawImage(target, m_treatmentOverlayImage);
        painter.restore();
    }

    painter.setPen(QPen(QColor(0, 0, 0, 150), 1.0));
    painter.drawRect(target);

    const QPainterPath &displaySelectionPath = m_selectionPreviewActive
        ? m_selectionPreviewBoundaryPath : m_selectionBoundaryPath;
    const QSize displaySelectionSize = m_selectionPreviewActive
        ? m_selectionPreviewCoverageSize : m_selectionCoverageSize;
    if ((m_selectionActive || m_selectionPreviewActive)
        && m_selectionEdgesVisible && !displaySelectionPath.isEmpty()
        && !displaySelectionSize.isEmpty()) {
        painter.save();
        QTransform selectionToViewport;
        selectionToViewport.translate(target.left(), target.top());
        selectionToViewport.scale(
            target.width() / std::max(1, displaySelectionSize.width()),
            target.height() / std::max(1, displaySelectionSize.height()));
        painter.setWorldTransform(selectionToViewport, true);
        painter.setRenderHint(QPainter::Antialiasing, false);

        QPen shadowPen(QColor(0, 0, 0, 235), 3.0);
        shadowPen.setCosmetic(true);
        painter.setPen(shadowPen);
        painter.drawPath(displaySelectionPath);

        QPen antsPen(QColor(255, 255, 255, 245), 1.0);
        antsPen.setCosmetic(true);
        antsPen.setStyle(Qt::CustomDashLine);
        antsPen.setDashPattern({4.0, 4.0});
        antsPen.setDashOffset(-m_selectionAntsPhase);
        painter.setPen(antsPen);
        painter.drawPath(displaySelectionPath);
        painter.restore();
    }

    if (m_selectionMarqueeDragging) {
        painter.save();
        if (m_selectionMarqueeClipToImage) {
            painter.setClipRect(target);
        } else {
            painter.setClipRect(viewport()->rect());
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);

        const QRectF bounds = currentSelectionMarqueeBounds();
        QPainterPath documentPath;
        const auto regularVertices = [this, &bounds](const bool star) {
            QPolygonF vertices;
            const int count = star ? m_selectionMarqueePolygonSides * 2
                                   : m_selectionMarqueePolygonSides;
            const QPointF centre = bounds.center();
            const double rx = bounds.width() * 0.5;
            const double ry = bounds.height() * 0.5;
            const double rotation = m_selectionMarqueeRotationDegrees
                * 3.14159265358979323846 / 180.0;
            for (int index = 0; index < count; ++index) {
                const double angle = rotation
                    + 2.0 * 3.14159265358979323846 * index / count;
                const double ratio = star && (index % 2 == 1)
                    ? m_selectionMarqueeStarInnerRatio : 1.0;
                vertices << QPointF(centre.x() + std::cos(angle) * rx * ratio,
                                    centre.y() + std::sin(angle) * ry * ratio);
            }
            return vertices;
        };
        switch (m_selectionMarqueePreviewMode) {
        case CanvasMarqueePreviewMode::Ellipse:
            documentPath.addEllipse(bounds);
            break;
        case CanvasMarqueePreviewMode::RoundedRectangle: {
            const double radius = std::min(m_selectionMarqueeCornerRadius,
                                           std::min(bounds.width(), bounds.height()) * 0.5);
            documentPath.addRoundedRect(bounds, radius, radius, Qt::AbsoluteSize);
            break;
        }
        case CanvasMarqueePreviewMode::Line: {
            const QLineF line = currentSelectionMarqueeLine();
            documentPath.moveTo(line.p1());
            documentPath.lineTo(line.p2());
            break;
        }
        case CanvasMarqueePreviewMode::Polygon:
        case CanvasMarqueePreviewMode::Star: {
            const QPolygonF vertices = regularVertices(
                m_selectionMarqueePreviewMode == CanvasMarqueePreviewMode::Star);
            if (!vertices.isEmpty()) {
                documentPath.moveTo(vertices.first());
                for (int index = 1; index < vertices.size(); ++index)
                    documentPath.lineTo(vertices.at(index));
                documentPath.closeSubpath();
            }
            break;
        }
        case CanvasMarqueePreviewMode::Arrow: {
            const double headStart = bounds.right()
                - bounds.width() * m_selectionMarqueeArrowHeadLengthRatio;
            const double halfShaft = bounds.height()
                * m_selectionMarqueeArrowShaftWidthRatio * 0.5;
            const double centreY = bounds.center().y();
            documentPath.moveTo(bounds.left(), centreY - halfShaft);
            documentPath.lineTo(headStart, centreY - halfShaft);
            documentPath.lineTo(headStart, bounds.top());
            documentPath.lineTo(bounds.right(), centreY);
            documentPath.lineTo(headStart, bounds.bottom());
            documentPath.lineTo(headStart, centreY + halfShaft);
            documentPath.lineTo(bounds.left(), centreY + halfShaft);
            documentPath.closeSubpath();
            break;
        }
        case CanvasMarqueePreviewMode::Rectangle:
        default:
            documentPath.addRect(bounds);
            break;
        }
        const QPainterPath marqueePath = documentToViewportTransform().map(documentPath);

        QPen shadowPen(QColor(0, 0, 0, 235), 3.0);
        shadowPen.setCosmetic(true);
        painter.setPen(shadowPen);
        painter.drawPath(marqueePath);

        QPen marqueePen(QColor(255, 255, 255, 245), 1.0);
        marqueePen.setCosmetic(true);
        marqueePen.setStyle(Qt::DashLine);
        painter.setPen(marqueePen);
        painter.drawPath(marqueePath);
        painter.restore();
    }

    if (m_selectionLassoActive) {
        painter.save();
        painter.setClipRect(target);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);

        const QPainterPath documentPath = currentSelectionLassoPath(true, true);
        const QPainterPath lassoPath = documentToViewportTransform().map(documentPath);
        if (!lassoPath.isEmpty()) {
            QPen shadowPen(QColor(0, 0, 0, 235), 3.0);
            shadowPen.setCosmetic(true);
            painter.setPen(shadowPen);
            painter.drawPath(lassoPath);

            QPen lassoPen(QColor(255, 255, 255, 245), 1.0);
            lassoPen.setCosmetic(true);
            lassoPen.setStyle(Qt::DashLine);
            painter.setPen(lassoPen);
            painter.drawPath(lassoPath);
        }

        if (m_selectionLassoMode == CanvasSelectionLassoMode::Polygonal
            && !m_selectionLassoViewportPoints.isEmpty()) {
            const QPointF first = m_selectionLassoViewportPoints.constFirst();
            const QRectF marker(first - QPointF(4.0, 4.0), QSizeF(8.0, 8.0));
            painter.setPen(QPen(QColor(0, 0, 0, 235), 3.0));
            painter.setBrush(QColor(255, 255, 255, 245));
            painter.drawEllipse(marker);
            painter.setPen(QPen(QColor(255, 255, 255, 245), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(marker);
        }
        painter.restore();
    }

    if (m_transformDragEnabled && !m_transformDocumentBounds.isEmpty()) {
        const QPolygonF box = transformBoxPolygon(m_transformCurrentTransform);
        if (box.size() == 4) {
            const QVector<QPointF> handles = transformHandlePoints(box);
            const QColor transformAccent = m_transformPendingChanges
                ? QColor(QStringLiteral("#f4b860"))
                : QColor(255, 255, 255, 245);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(0, 0, 0, 210), 3.0));
            painter.drawPolygon(box);
            painter.setPen(QPen(transformAccent, 1.0));
            painter.drawPolygon(box);
            if (handles.size() == 9) {
                const bool affineMode = m_transformInteractionMode
                        == CanvasTransformInteractionMode::FreeTransform
                    || m_transformInteractionMode == CanvasTransformInteractionMode::Scale
                    || m_transformInteractionMode == CanvasTransformInteractionMode::Rotate;
                if (affineMode) {
                    const bool showScale = m_transformInteractionMode
                        != CanvasTransformInteractionMode::Rotate;
                    const bool showRotate = m_transformInteractionMode
                        != CanvasTransformInteractionMode::Scale;
                    if (showScale) {
                        for (int index = 0; index < 8; ++index) {
                            const QRectF handle(handles.at(index) - QPointF(4.0, 4.0),
                                                QSizeF(8.0, 8.0));
                            painter.fillRect(handle, transformAccent);
                            painter.setPen(QPen(themeColour(QStringLiteral("preview_background")), 1.0));
                            painter.drawRect(handle);
                            painter.setPen(QPen(transformAccent, 1.0));
                        }
                    }
                    if (showRotate) {
                        painter.setPen(QPen(transformAccent, 1.0));
                        painter.drawLine(handles.at(1), handles.at(8));
                        painter.setBrush(themeColour(QStringLiteral("preview_background")));
                        painter.drawEllipse(handles.at(8), 5.0, 5.0);
                    }

                    const QPointF pivotViewport = documentToViewportTransform().map(
                        transformPivot());
                    painter.setPen(QPen(QColor(0, 0, 0, 220), 3.0));
                    painter.drawLine(pivotViewport - QPointF(7.0, 0.0),
                                     pivotViewport + QPointF(7.0, 0.0));
                    painter.drawLine(pivotViewport - QPointF(0.0, 7.0),
                                     pivotViewport + QPointF(0.0, 7.0));
                    painter.setPen(QPen(transformAccent, 1.0));
                    painter.drawEllipse(pivotViewport, 4.0, 4.0);
                    painter.drawLine(pivotViewport - QPointF(6.0, 0.0),
                                     pivotViewport + QPointF(6.0, 0.0));
                    painter.drawLine(pivotViewport - QPointF(0.0, 6.0),
                                     pivotViewport + QPointF(0.0, 6.0));
                } else if (m_transformInteractionMode
                           == CanvasTransformInteractionMode::Skew) {
                    static constexpr int edgeHandles[] = {1, 3, 5, 7};
                    painter.setBrush(transformAccent);
                    for (const int index : edgeHandles) {
                        const QPointF point = handles.at(index);
                        QPolygonF diamond;
                        diamond << point + QPointF(0.0, -6.0)
                                << point + QPointF(6.0, 0.0)
                                << point + QPointF(0.0, 6.0)
                                << point + QPointF(-6.0, 0.0);
                        painter.setPen(QPen(themeColour(QStringLiteral("preview_background")), 1.0));
                        painter.drawPolygon(diamond);
                    }
                } else {
                    static constexpr int cornerHandles[] = {0, 2, 4, 6};
                    const bool perspective = m_transformInteractionMode
                        == CanvasTransformInteractionMode::Perspective;
                    for (const int index : cornerHandles) {
                        const QRectF handle(handles.at(index) - QPointF(5.0, 5.0),
                                            QSizeF(10.0, 10.0));
                        painter.setBrush(perspective
                            ? themeColour(QStringLiteral("preview_background"))
                            : transformAccent);
                        painter.setPen(QPen(transformAccent,
                                            perspective ? 2.0 : 1.0));
                        painter.drawRect(handle);
                    }
                }
            }
            painter.restore();
        }
    }

    if (m_paintMode != CanvasPaintMode::None
        && m_paintMode != CanvasPaintMode::Patch
        && m_paintMode != CanvasPaintMode::Fill
        && m_paintMode != CanvasPaintMode::Gradient
        && m_mouseOverViewport && !m_panning
        && !m_spaceHeld && !m_draggingGuide && hasImage()) {
        const bool cloneSampling = (m_paintMode == CanvasPaintMode::CloneStamp
             || m_paintMode == CanvasPaintMode::HealingBrush)
            && QApplication::keyboardModifiers().testFlag(Qt::AltModifier);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        if (cloneSampling) {
            const QPointF point = m_lastMouseViewportPosition;
            painter.setPen(QPen(QColor(0, 0, 0, 220), 3.0));
            painter.drawEllipse(point, 7.0, 7.0);
            painter.drawLine(point - QPointF(11.0, 0.0), point + QPointF(11.0, 0.0));
            painter.drawLine(point - QPointF(0.0, 11.0), point + QPointF(0.0, 11.0));
            painter.setPen(QPen(QColor(255, 255, 255, 235), 1.0));
            painter.drawEllipse(point, 7.0, 7.0);
            painter.drawLine(point - QPointF(11.0, 0.0), point + QPointF(11.0, 0.0));
            painter.drawLine(point - QPointF(0.0, 11.0), point + QPointF(0.0, 11.0));
        } else {
            const double radius = std::max(0.5, m_brushDiameter * 0.5
                * documentScale(Qt::Vertical));
            painter.setPen(QPen(QColor(0, 0, 0, 210), 3.0));
            painter.drawEllipse(m_lastMouseViewportPosition, radius, radius);
            painter.setPen(QPen(QColor(255, 255, 255, 225), 1.0));
            painter.drawEllipse(m_lastMouseViewportPosition, radius, radius);
        }
        painter.restore();
    }

    if (m_gradientOverlayVisible && hasImage()) {
        const QPointF start = mapDocumentToViewport(m_gradientStartDocument);
        const QPointF end = mapDocumentToViewport(m_gradientEndDocument);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 220), 3.0));
        painter.drawLine(start, end);
        painter.drawEllipse(start, 6.0, 6.0);
        painter.drawEllipse(end, 6.0, 6.0);
        painter.setPen(QPen(QColor(255, 255, 255, 245), 1.0));
        painter.drawLine(start, end);
        painter.setBrush(QColor(255, 255, 255, 245));
        painter.drawEllipse(start, 4.5, 4.5);
        painter.setBrush(QColor(255, 196, 72, 245));
        painter.drawEllipse(end, 4.5, 4.5);
        painter.restore();
    }

    if ((m_paintMode == CanvasPaintMode::CloneStamp
         || m_paintMode == CanvasPaintMode::HealingBrush)
        && m_cloneSourceMarkerVisible
        && hasImage()) {
        const QRectF target = imageRect();
        const QPointF preview = previewPositionForDocument(m_cloneSourceDocumentPosition);
        const QPointF point(target.left() + preview.x() * target.width()
                                / std::max(1, m_image.width()),
                            target.top() + preview.y() * target.height()
                                / std::max(1, m_image.height()));
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 225), 3.0));
        painter.drawEllipse(point, 6.0, 6.0);
        painter.drawLine(point - QPointF(10.0, 0.0), point + QPointF(10.0, 0.0));
        painter.drawLine(point - QPointF(0.0, 10.0), point + QPointF(0.0, 10.0));
        painter.setPen(QPen(QColor(255, 210, 96, 245), 1.0));
        painter.drawEllipse(point, 6.0, 6.0);
        painter.drawLine(point - QPointF(10.0, 0.0), point + QPointF(10.0, 0.0));
        painter.drawLine(point - QPointF(0.0, 10.0), point + QPointF(0.0, 10.0));
        painter.restore();
    }

    if (rotateCropPreview) {
        painter.restore();
    }

    paintVignetteOverlay(painter);
    paintVectorPathOverlay(painter);

    if (m_guidesVisible) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QPen guidePen(QColor(QStringLiteral("#43c8ff")), 1.0);
        painter.setPen(guidePen);

        for (int index = 0; index < m_verticalGuides.size(); ++index) {
            if (m_draggingGuide && !m_draggingNewGuide
                && m_dragGuideOrientation == Qt::Vertical && m_dragGuideIndex == index) {
                continue;
            }
            const double x = viewportCoordinate(Qt::Vertical, m_verticalGuides.at(index));
            painter.drawLine(QPointF(x, 0.0), QPointF(x, viewport()->height()));
        }
        for (int index = 0; index < m_horizontalGuides.size(); ++index) {
            if (m_draggingGuide && !m_draggingNewGuide
                && m_dragGuideOrientation == Qt::Horizontal && m_dragGuideIndex == index) {
                continue;
            }
            const double y = viewportCoordinate(Qt::Horizontal, m_horizontalGuides.at(index));
            painter.drawLine(QPointF(0.0, y), QPointF(viewport()->width(), y));
        }

        if (m_draggingGuide) {
            painter.setPen(QPen(m_dragGuideSnapped
                                    ? QColor(QStringLiteral("#ffd166"))
                                    : QColor(QStringLiteral("#78dcff")),
                                1.0));
            const double position = viewportCoordinate(m_dragGuideOrientation,
                                                       m_dragGuidePosition);
            if (m_dragGuideOrientation == Qt::Vertical) {
                painter.drawLine(QPointF(position, 0.0),
                                 QPointF(position, viewport()->height()));
            } else {
                painter.drawLine(QPointF(0.0, position),
                                 QPointF(viewport()->width(), position));
            }
        }
        painter.restore();
    }

    if (m_transformDragging && (m_transformSnapXActive || m_transformSnapYActive)) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        const auto drawIndicator = [&painter](const QLineF &line) {
            painter.setPen(QPen(QColor(20, 17, 26, 210), 3.0));
            painter.drawLine(line);
            painter.setPen(QPen(QColor(QStringLiteral("#ffd166")), 1.0));
            painter.drawLine(line);
        };
        if (m_transformSnapXActive) {
            const double x = viewportCoordinate(Qt::Vertical, m_transformSnapXTarget);
            drawIndicator(QLineF(QPointF(x, 0.0), QPointF(x, viewport()->height())));
        }
        if (m_transformSnapYActive) {
            const double y = viewportCoordinate(Qt::Horizontal, m_transformSnapYTarget);
            drawIndicator(QLineF(QPointF(0.0, y), QPointF(viewport()->width(), y)));
        }
        painter.restore();
    }

    paintCropOverlay(painter);
}

void ImageCanvas::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateRulerGeometry();
    if (m_fitMode) {
        fitToView();
    } else {
        const QSignalBlocker horizontalBlock(horizontalScrollBar());
        const QSignalBlocker verticalBlock(verticalScrollBar());
        updateScrollBars();
        updateRulers();
        notifyPresentationViewportChanged(false);
    }
}

void ImageCanvas::wheelEvent(QWheelEvent *event)
{
    if (!hasImage()) {
        event->ignore();
        return;
    }

    const double steps = event->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(steps)) {
        event->ignore();
        return;
    }

    setZoom(m_zoom * std::pow(1.18, steps), event->position());
    event->accept();
}

void ImageCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_vectorPathEditingEnabled
        && !m_spaceHeld && hasImage()) {
        m_vectorPathPointerDown = true;
        emit vectorPathPointerPressed(documentEdgePositionAtUnclamped(event->position()),
                                      event->modifiers());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_colourSamplingEnabled && !m_spaceHeld) {
        if (sampleColourAt(event->position())) {
            m_samplingColour = true;
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::LeftButton && m_cropEnabled
        && !m_spaceHeld && hasImage()) {
        const QPointF documentPosition = documentEdgePositionAtUnclamped(
            event->position());
        if (m_cropStraightenSampling) {
            m_cropStraightenDragging = true;
            m_cropStraightenStart = documentPosition;
            m_cropStraightenCurrent = documentPosition;
            viewport()->update();
            event->accept();
            return;
        }
        m_cropDragMode = cropModeAt(event->position());
        m_cropDragging = true;
        m_cropDragStartFrame = m_cropFrame;
        m_cropDragStartDocument = documentPosition;
        m_cropCreateAnchor = documentPosition;
        m_cropCreateRepositioning = false;
        if (m_cropDragMode == CropDragMode::Create
            && m_cropConstraintMode == CropMode::FixedSize
            && m_cropFixedSize.isValid()) {
            m_cropFrame = QRectF(documentPosition, QSizeF(m_cropFixedSize));
            m_cropDragMode = CropDragMode::Move;
            m_cropDragStartFrame = m_cropFrame;
        }
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_vignetteOverlayVisible
        && !m_spaceHeld && hasImage()) {
        const VignetteDragMode mode = vignetteModeAt(event->position());
        if (mode != VignetteDragMode::None) {
            m_vignetteDragging = true;
            m_vignetteDragMode = mode;
            m_vignetteDragStartOverlay = m_vignetteOverlay;
            if (m_vignetteOverlayInteractionStarted) {
                m_vignetteOverlayInteractionStarted();
            }
            updateVignetteCursor(event->position());
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::LeftButton
        && (m_paintMode == CanvasPaintMode::CloneStamp
            || m_paintMode == CanvasPaintMode::HealingBrush)
        && event->modifiers().testFlag(Qt::AltModifier)
        && !m_spaceHeld && hasImage()) {
        const std::optional<QPointF> documentPosition = documentPositionAt(event->position());
        if (documentPosition.has_value()) {
            emit cloneSourceSampleRequested(*documentPosition);
        }
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_paintMode != CanvasPaintMode::None
        && !m_spaceHeld && hasImage()) {
        const QPointF documentPosition = documentPositionAtUnclamped(event->position());
        m_painting = true;
        m_paintPositionValid = true;
        m_lastPaintDocumentPosition = documentPosition;
        emit paintStrokeStarted(documentPosition);
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton
        && m_selectionLassoMode != CanvasSelectionLassoMode::None
        && !m_spaceHeld && hasImage()) {
        if (m_selectionLassoMode == CanvasSelectionLassoMode::Freehand) {
            // Selection gestures may begin anywhere in the canvas viewport.
            // Document-space geometry is allowed to extend outside the image
            // and is clipped only when the selection is committed. A tiny
            // plain click still resolves to Deselect in mouseReleaseEvent().
            const QPointF documentPosition =
                documentEdgePositionAtUnclamped(event->position());

            m_selectionLassoActive = true;
            m_selectionLassoPointerDown = true;
            m_selectionLassoPoints.clear();
            m_selectionLassoViewportPoints.clear();
            m_selectionLassoPressViewportPosition = event->position();
            m_selectionLassoStartModifiers = event->modifiers();
            appendFreehandLassoPoint(documentPosition, event->position(), true);
            emit selectionLassoStarted(currentSelectionLassoPath(false, true),
                                       event->modifiers());
            viewport()->update(selectionLassoViewportBounds());
            event->accept();
            return;
        }

        if (!m_selectionLassoActive) {
            // Polygonal selection vertices may start in the overscroll void;
            // the final path is clipped to the document by SelectionMask.
            const QPointF documentPosition =
                documentEdgePositionAtUnclamped(event->position());

            m_selectionLassoActive = true;
            m_selectionLassoPointerDown = false;
            m_selectionLassoPoints.clear();
            m_selectionLassoViewportPoints.clear();
            m_selectionLassoPoints.push_back(documentPosition);
            m_selectionLassoViewportPoints.push_back(event->position());
            m_selectionLassoCurrent = documentPosition;
            m_selectionLassoCurrentViewport = event->position();
            m_selectionLassoPressViewportPosition = event->position();
            m_selectionLassoStartModifiers = event->modifiers();
            emit selectionLassoStarted(currentSelectionLassoPath(true, true),
                                       event->modifiers());
            viewport()->update(selectionLassoViewportBounds());
            event->accept();
            return;
        }

        const QPointF documentPosition =
            documentEdgePositionAtUnclamped(event->position());
        m_selectionLassoCurrent = documentPosition;
        m_selectionLassoCurrentViewport = event->position();
        if (m_selectionLassoPoints.size() >= 3
            && QLineF(m_selectionLassoViewportPoints.constFirst(),
                      event->position()).length() <= PolygonalLassoCloseDistance) {
            finishSelectionLassoGesture();
            event->accept();
            return;
        }
        if (m_selectionLassoViewportPoints.isEmpty()
            || QLineF(m_selectionLassoViewportPoints.constLast(),
                      event->position()).length() >= 1.0) {
            m_selectionLassoPoints.push_back(documentPosition);
            m_selectionLassoViewportPoints.push_back(event->position());
        }
        emit selectionLassoChanged(currentSelectionLassoPath(true, true));
        viewport()->update(selectionLassoViewportBounds());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_textToolHitTestingEnabled
        && !m_spaceHeld && hasImage()) {
        const QPointF documentPosition =
            documentEdgePositionAtUnclamped(event->position());
        emit textToolPressed(documentPosition, event->modifiers());
        if (m_textEditingActive) {
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::LeftButton && m_selectionMarqueeEnabled
        && !m_spaceHeld && hasImage()) {
        // Marquees may begin in the grey overscroll area and cross into or
        // across the image. The path stays in unclamped document coordinates
        // during interaction and SelectionMask clips it at commit time. A
        // tiny plain click anywhere in the viewport still requests Deselect.
        const QPointF documentPosition =
            documentEdgePositionAtUnclamped(event->position());
        m_selectionMarqueeDragging = true;
        m_selectionMarqueeRepositioning = false;
        m_selectionMarqueeStart = documentPosition;
        m_selectionMarqueeCurrent = documentPosition;
        m_selectionMarqueeLastRepositionPoint = documentPosition;
        m_selectionMarqueePressViewportPosition = event->position();
        m_selectionMarqueeStartModifiers = event->modifiers();
        m_selectionMarqueeCurrentModifiers = event->modifiers();
        emit selectionMarqueeStarted(currentSelectionMarqueeBounds(),
                                      event->modifiers());
        viewport()->update();
        event->accept();
        return;
    }

    if (event->button() == Qt::RightButton && m_guidesVisible) {
        const GuideHit hit = guideAt(event->position(), GuideHitTolerance);
        if (hit.isValid()) {
            removeGuide(hit);
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::LeftButton && m_guidesVisible && !m_spaceHeld) {
        const GuideHit hit = guideAt(event->position(), GuideHitTolerance);
        if (hit.isValid()) {
            beginExistingGuideDrag(hit);
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::RightButton && m_transformDragEnabled
        && !m_transformDocumentBounds.isEmpty() && !m_spaceHeld) {
        const QPolygonF box = transformBoxPolygon(m_transformCurrentTransform);
        if (box.containsPoint(event->position(), Qt::OddEvenFill)) {
            emit transformContextMenuRequested(event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }

    if (event->button() == Qt::LeftButton && m_transformDragEnabled && !m_spaceHeld) {
        const CanvasTransformMode mode = transformModeAt(event->position());
        if (mode != CanvasTransformMode::None) {
            // Transform bounds/handles live in document-edge coordinates (0..width,
            // 0..height), matching documentToViewportTransform().  Using the
            // pixel-centre inverse (0..width-1) subtly shrinks every pointer
            // delta and makes transform gestures size-dependent.
            const QPointF documentPosition =
                documentEdgePositionAtUnclamped(event->position());
            m_transformGestureBaseTransform = m_transformCurrentTransform;
            m_transformGestureBaseQuad = transformDocumentQuad(
                m_transformGestureBaseTransform);
            m_transformGestureStartPivotDocument = transformPivot();
            if (mode == CanvasTransformMode::Pivot) {
                m_transformPivotDragging = true;
                m_transformMode = mode;
                viewport()->setCursor(Qt::CrossCursor);
                event->accept();
                return;
            }
            m_transformDragging = true;
            m_transformMode = mode;
            m_transformStartDocumentPosition = documentPosition;
            clearTransformSnapState();
            const QRectF bounds = m_transformDocumentBounds;
            switch (mode) {
            case CanvasTransformMode::ScaleTopLeft:
                m_transformAnchorDocumentPosition = bounds.bottomRight();
                m_transformStartHandleDocumentPosition = bounds.topLeft();
                break;
            case CanvasTransformMode::ScaleTop:
                m_transformAnchorDocumentPosition = QPointF(bounds.center().x(), bounds.bottom());
                m_transformStartHandleDocumentPosition = QPointF(bounds.center().x(), bounds.top());
                break;
            case CanvasTransformMode::ScaleTopRight:
                m_transformAnchorDocumentPosition = bounds.bottomLeft();
                m_transformStartHandleDocumentPosition = bounds.topRight();
                break;
            case CanvasTransformMode::ScaleRight:
                m_transformAnchorDocumentPosition = QPointF(bounds.left(), bounds.center().y());
                m_transformStartHandleDocumentPosition = QPointF(bounds.right(), bounds.center().y());
                break;
            case CanvasTransformMode::ScaleBottomRight:
                m_transformAnchorDocumentPosition = bounds.topLeft();
                m_transformStartHandleDocumentPosition = bounds.bottomRight();
                break;
            case CanvasTransformMode::ScaleBottom:
                m_transformAnchorDocumentPosition = QPointF(bounds.center().x(), bounds.top());
                m_transformStartHandleDocumentPosition = QPointF(bounds.center().x(), bounds.bottom());
                break;
            case CanvasTransformMode::ScaleBottomLeft:
                m_transformAnchorDocumentPosition = bounds.topRight();
                m_transformStartHandleDocumentPosition = bounds.bottomLeft();
                break;
            case CanvasTransformMode::ScaleLeft:
                m_transformAnchorDocumentPosition = QPointF(bounds.right(), bounds.center().y());
                m_transformStartHandleDocumentPosition = QPointF(bounds.left(), bounds.center().y());
                break;
            case CanvasTransformMode::SkewTop:
            case CanvasTransformMode::SkewRight:
            case CanvasTransformMode::SkewBottom:
            case CanvasTransformMode::SkewLeft:
            case CanvasTransformMode::ControlTopLeft:
            case CanvasTransformMode::ControlTopRight:
            case CanvasTransformMode::ControlBottomRight:
            case CanvasTransformMode::ControlBottomLeft:
            case CanvasTransformMode::Move:
            case CanvasTransformMode::Rotate:
            case CanvasTransformMode::Pivot:
            case CanvasTransformMode::None:
                m_transformAnchorDocumentPosition = bounds.center();
                m_transformStartHandleDocumentPosition = documentPosition;
                break;
            }
            const int controlPoint = transformControlPointIndex(mode);
            if (controlPoint >= 0) {
                emit transformControlPointSelected(controlPoint);
            }
            emit transformDragStarted(mode,
                                      documentPosition,
                                      event->modifiers());
            event->accept();
            return;
        }
    }

    if (panGestureActive(event)) {
        if (m_vectorPathEditingEnabled) emit vectorPathPointerLeft();
        m_panning = true;
        m_lastPanPosition = event->position().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void ImageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF previousMousePosition = m_lastMouseViewportPosition;
    // An active pan owns the pointer regardless of the current editing tool.
    // Pen, Direct Selection and Corner Tool previously consumed every move
    // before this branch, so middle-button panning started but never moved.
    if (m_panning) {
        const QPoint position = event->position().toPoint();
        const QPoint delta = position - m_lastPanPosition;
        {
            const QSignalBlocker horizontalBlock(horizontalScrollBar());
            const QSignalBlocker verticalBlock(verticalScrollBar());
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        }
        m_lastPanPosition = position;
        m_mouseOverViewport = true;
        m_lastMouseViewportPosition = event->position();
        viewport()->update();
        updateRulers();
        notifyPresentationViewportChanged(false);
        event->accept();
        return;
    }
    if (m_vectorPathEditingEnabled && !m_spaceHeld && hasImage()) {
        emit vectorPathPointerMoved(documentEdgePositionAtUnclamped(event->position()),
                                    event->buttons(), event->modifiers());
        if (m_vectorPathPointerDown || (event->buttons() & Qt::LeftButton)) {
            event->accept();
            return;
        }
        m_mouseOverViewport = true;
        m_lastMouseViewportPosition = event->position();
        event->accept();
        return;
    }
    m_mouseOverViewport = true;
    m_lastMouseViewportPosition = event->position();

    if (m_cropStraightenDragging && (event->buttons() & Qt::LeftButton)) {
        m_cropStraightenCurrent = documentEdgePositionAtUnclamped(event->position());
        viewport()->update();
        event->accept();
        return;
    }
    if (m_cropDragging && (event->buttons() & Qt::LeftButton)) {
        const QPointF documentPosition = documentEdgePositionAtUnclamped(event->position());
        if (m_cropCreateRepositioning) {
            m_cropFrame = pixelAlignedCropFrame(
                m_cropCreateRepositionStartFrame.translated(
                    documentPosition - m_cropCreateRepositionStartDocument));
        } else {
            m_cropFrame = cropFrameFromPointer(documentPosition, event->modifiers());
        }
        emit cropFrameChanged(m_cropFrame);
        viewport()->update();
        event->accept();
        return;
    }

    if (m_vignetteDragging && (event->buttons() & Qt::LeftButton)) {
        updateVignetteFromPointer(
            documentEdgePositionAtUnclamped(event->position()));
        updateVignetteCursor(event->position());
        event->accept();
        return;
    }

    if (m_painting && (event->buttons() & Qt::LeftButton)) {
        const QPointF documentPosition = documentPositionAtUnclamped(event->position());
        if (m_paintPositionValid) {
            const QPointF previous = m_lastPaintDocumentPosition;
            m_lastPaintDocumentPosition = documentPosition;
            emit paintStrokeContinued(previous, documentPosition);
        } else {
            m_lastPaintDocumentPosition = documentPosition;
            m_paintPositionValid = true;
        }
        const double radius = std::max(0.5, m_brushDiameter * 0.5
            * documentScale(Qt::Vertical)) + 5.0;
        const QRect previousRect = QRectF(previousMousePosition - QPointF(radius, radius),
                                           QSizeF(radius * 2.0, radius * 2.0))
                                       .toAlignedRect();
        const QRect currentRect = QRectF(m_lastMouseViewportPosition - QPointF(radius, radius),
                                          QSizeF(radius * 2.0, radius * 2.0))
                                      .toAlignedRect();
        viewport()->update(previousRect.united(currentRect));
        event->accept();
        return;
    }

    if (m_selectionLassoActive
        && m_selectionLassoMode == CanvasSelectionLassoMode::Freehand
        && m_selectionLassoPointerDown
        && (event->buttons() & Qt::LeftButton)) {
        const QPointF oldLast = m_selectionLassoViewportPoints.isEmpty()
            ? event->position() : m_selectionLassoViewportPoints.constLast();
        const QPointF oldPrevious = m_selectionLassoViewportPoints.size() >= 2
            ? m_selectionLassoViewportPoints.at(m_selectionLassoViewportPoints.size() - 2)
            : oldLast;
        const QPointF first = m_selectionLassoViewportPoints.isEmpty()
            ? oldLast : m_selectionLassoViewportPoints.constFirst();
        const QPointF documentPosition =
            documentEdgePositionAtUnclamped(event->position());
        if (appendFreehandLassoPoint(documentPosition, event->position())) {
            emit selectionLassoChanged(currentSelectionLassoPath(false, true));
            const QPointF newLast = m_selectionLassoViewportPoints.constLast();
            QRectF dirty(oldPrevious, oldLast);
            dirty = dirty.normalized().united(QRectF(oldPrevious, newLast).normalized());
            dirty = dirty.united(QRectF(first, oldLast).normalized());
            dirty = dirty.united(QRectF(first, newLast).normalized());
            viewport()->update(dirty.toAlignedRect().adjusted(-6, -6, 6, 6));
        }
        event->accept();
        return;
    }

    if (m_selectionLassoActive
        && m_selectionLassoMode == CanvasSelectionLassoMode::Polygonal
        && !(event->buttons() & Qt::LeftButton)) {
        const QPointF oldCurrent = m_selectionLassoCurrentViewport;
        m_selectionLassoCurrent = documentEdgePositionAtUnclamped(event->position());
        m_selectionLassoCurrentViewport = event->position();
        emit selectionLassoChanged(currentSelectionLassoPath(true, true));
        if (!m_selectionLassoViewportPoints.isEmpty()) {
            const QPointF last = m_selectionLassoViewportPoints.constLast();
            const QPointF first = m_selectionLassoViewportPoints.constFirst();
            QRectF dirty(last, oldCurrent);
            dirty = dirty.normalized().united(QRectF(last, event->position()).normalized());
            dirty = dirty.united(QRectF(first, oldCurrent).normalized());
            dirty = dirty.united(QRectF(first, event->position()).normalized());
            dirty = dirty.united(QRectF(first - QPointF(7.0, 7.0),
                                        QSizeF(14.0, 14.0)));
            viewport()->update(dirty.toAlignedRect().adjusted(-6, -6, 6, 6));
        }
        event->accept();
        return;
    }

    if (m_selectionMarqueeDragging && (event->buttons() & Qt::LeftButton)) {
        const QRectF previousBounds = currentSelectionMarqueeBounds();
        m_selectionMarqueeCurrentModifiers = event->modifiers();
        const QPointF documentPosition =
            documentEdgePositionAtUnclamped(event->position());
        if (m_selectionMarqueeRepositioning) {
            const QPointF delta = documentPosition
                - m_selectionMarqueeLastRepositionPoint;
            m_selectionMarqueeStart += delta;
            m_selectionMarqueeCurrent += delta;
            m_selectionMarqueeLastRepositionPoint = documentPosition;
        } else {
            m_selectionMarqueeCurrent = documentPosition;
        }
        const QRectF currentBounds = currentSelectionMarqueeBounds();
        emit selectionMarqueeChanged(currentBounds);

        // Repaint only the old and new vector-outline regions. This avoids a
        // full tiled-canvas presentation pass for every pointer sample.
        const QTransform toViewport = documentToViewportTransform();
        const QRect dirty = toViewport.mapRect(previousBounds)
            .united(toViewport.mapRect(currentBounds))
            .toAlignedRect()
            .adjusted(-5, -5, 5, 5);
        viewport()->update(dirty);
        event->accept();
        return;
    }

    if (m_transformPivotDragging && (event->buttons() & Qt::LeftButton)) {
        m_transformPivotDocument =
            documentEdgePositionAtUnclamped(event->position());
        m_transformPivotValid = true;
        emit transformPivotChanged(m_transformPivotDocument);
        viewport()->update();
        event->accept();
        return;
    }

    if (m_transformDragging && (event->buttons() & Qt::LeftButton)) {
        const QPointF position =
            documentEdgePositionAtUnclamped(event->position());
        m_transformCurrentTransform = transformFromPointer(position, event->modifiers());
        emit transformDragChanged(m_transformCurrentTransform);
        event->accept();
        return;
    }

    if (m_samplingColour && (event->buttons() & Qt::LeftButton)) {
        sampleColourAt(event->position());
        event->accept();
        return;
    }

    if (m_draggingGuide && !m_draggingNewGuide) {
        updateExistingGuideDrag(event->position());
        event->accept();
        return;
    }

    if (!m_spaceHeld && m_cropEnabled) {
        updateCropCursor(event->position());
    } else if (!m_spaceHeld && m_vignetteOverlayVisible) {
        updateVignetteCursor(event->position());
    } else if (!m_spaceHeld && m_transformDragEnabled) {
        updateTransformCursor(event->position());
    } else if (!m_spaceHeld && m_guidesVisible) {
        const GuideHit hit = guideAt(event->position(), GuideHitTolerance);
        if (hit.orientation == Qt::Vertical && hit.isValid()) {
            viewport()->setCursor(Qt::SizeHorCursor);
        } else if (hit.orientation == Qt::Horizontal && hit.isValid()) {
            viewport()->setCursor(Qt::SizeVerCursor);
        } else {
            viewport()->setCursor(m_toolCursor);
        }
    }
    if (m_paintMode != CanvasPaintMode::None
        && m_paintMode != CanvasPaintMode::Fill) {
        const double radius = std::max(0.5, m_brushDiameter * 0.5
            * documentScale(Qt::Vertical)) + 5.0;
        const QRect previousRect = QRectF(previousMousePosition - QPointF(radius, radius),
                                           QSizeF(radius * 2.0, radius * 2.0))
                                       .toAlignedRect();
        const QRect currentRect = QRectF(m_lastMouseViewportPosition - QPointF(radius, radius),
                                          QSizeF(radius * 2.0, radius * 2.0))
                                      .toAlignedRect();
        viewport()->update(previousRect.united(currentRect));
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_vectorPathEditingEnabled
        && m_vectorPathPointerDown && hasImage()) {
        m_vectorPathPointerDown = false;
        emit vectorPathPointerReleased(documentEdgePositionAtUnclamped(event->position()),
                                       event->modifiers());
        event->accept();
        return;
    }
    if (m_cropStraightenDragging && event->button() == Qt::LeftButton) {
        m_cropStraightenCurrent = documentEdgePositionAtUnclamped(event->position());
        const QLineF line(m_cropStraightenStart, m_cropStraightenCurrent);
        m_cropStraightenDragging = false;
        if (line.length() >= 2.0) {
            emit cropStraightenLineFinished(line, event->modifiers());
        }
        viewport()->update();
        event->accept();
        return;
    }
    if (m_cropDragging && event->button() == Qt::LeftButton) {
        const QPointF documentPosition = documentEdgePositionAtUnclamped(event->position());
        if (m_cropCreateRepositioning) {
            const QPointF delta = documentPosition - m_cropCreateRepositionStartDocument;
            m_cropFrame = pixelAlignedCropFrame(
                m_cropCreateRepositionStartFrame.translated(delta));
            m_cropCreateAnchor += delta;
            m_cropCreateRepositioning = false;
        } else {
            m_cropFrame = cropFrameFromPointer(documentPosition, event->modifiers());
        }
        m_cropDragging = false;
        m_cropDragMode = CropDragMode::None;
        emit cropFrameChanged(m_cropFrame);
        emit cropFrameFinished(m_cropFrame);
        updateCropCursor(event->position());
        viewport()->update();
        event->accept();
        return;
    }

    if (m_vignetteDragging && event->button() == Qt::LeftButton) {
        updateVignetteFromPointer(
            documentEdgePositionAtUnclamped(event->position()));
        m_vignetteDragging = false;
        m_vignetteDragMode = VignetteDragMode::None;
        if (m_vignetteOverlayInteractionFinished) {
            m_vignetteOverlayInteractionFinished();
        }
        updateVignetteCursor(event->position());
        viewport()->update();
        event->accept();
        return;
    }

    if (m_painting && event->button() == Qt::LeftButton) {
        const QPointF documentPosition = documentPositionAtUnclamped(event->position());
        if (m_paintPositionValid) {
            emit paintStrokeContinued(m_lastPaintDocumentPosition, documentPosition);
            m_lastPaintDocumentPosition = documentPosition;
        }
        m_painting = false;
        m_paintPositionValid = false;
        emit paintStrokeFinished();
        viewport()->update();
        event->accept();
        return;
    }

    if (m_selectionLassoActive
        && m_selectionLassoMode == CanvasSelectionLassoMode::Freehand
        && m_selectionLassoPointerDown && event->button() == Qt::LeftButton) {
        const QPointF documentPosition =
            documentEdgePositionAtUnclamped(event->position());
        appendFreehandLassoPoint(documentPosition, event->position(), true);
        const bool plainClick = QLineF(m_selectionLassoPressViewportPosition,
                                       event->position()).length() < 3.0
            && !m_selectionLassoStartModifiers.testFlag(Qt::ShiftModifier)
            && !m_selectionLassoStartModifiers.testFlag(Qt::AltModifier);
        const bool tinyGesture = QLineF(m_selectionLassoPressViewportPosition,
                                        event->position()).length() < 3.0;
        if (tinyGesture) {
            m_selectionLassoActive = false;
            m_selectionLassoPointerDown = false;
            m_selectionLassoPoints.clear();
            m_selectionLassoViewportPoints.clear();
            if (plainClick) {
                emit selectionDeselectRequested();
            } else {
                emit selectionLassoCancelled();
            }
            viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
            viewport()->update();
            event->accept();
            return;
        }
        finishSelectionLassoGesture();
        event->accept();
        return;
    }

    if (m_selectionMarqueeDragging && event->button() == Qt::LeftButton) {
        m_selectionMarqueeCurrentModifiers = event->modifiers();
        const QPointF documentPosition =
            documentEdgePositionAtUnclamped(event->position());
        if (m_selectionMarqueeRepositioning) {
            const QPointF delta = documentPosition
                - m_selectionMarqueeLastRepositionPoint;
            m_selectionMarqueeStart += delta;
            m_selectionMarqueeCurrent += delta;
        } else {
            m_selectionMarqueeCurrent = documentPosition;
        }

        const bool tinyGesture = QLineF(m_selectionMarqueePressViewportPosition,
                                        event->position()).length() < 3.0;
        const bool plainClick = tinyGesture
            && !m_selectionMarqueeStartModifiers.testFlag(Qt::ShiftModifier)
            && !m_selectionMarqueeStartModifiers.testFlag(Qt::AltModifier);
        if (tinyGesture) {
            m_selectionMarqueeDragging = false;
            m_selectionMarqueeRepositioning = false;
            if (plainClick && m_selectionMarqueeFinishOnClick) {
                emit selectionMarqueeFinished(QRectF(m_selectionMarqueeStart, QSizeF(1.0, 1.0)));
            } else if (plainClick && m_selectionMarqueeDeselectOnClick) {
                emit selectionDeselectRequested();
            } else {
                emit selectionMarqueeCancelled();
            }
            viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
            viewport()->update();
            event->accept();
            return;
        }

        const QRectF bounds = currentSelectionMarqueeBounds();
        m_selectionMarqueeDragging = false;
        m_selectionMarqueeRepositioning = false;
        emit selectionMarqueeFinished(bounds);
        viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
        viewport()->update();
        event->accept();
        return;
    }

    if (m_transformPivotDragging && event->button() == Qt::LeftButton) {
        m_transformPivotDocument =
            documentEdgePositionAtUnclamped(event->position());
        m_transformPivotValid = true;
        m_transformPivotDragging = false;
        m_transformMode = CanvasTransformMode::None;
        emit transformPivotChanged(m_transformPivotDocument);
        updateTransformCursor(event->position());
        viewport()->update();
        event->accept();
        return;
    }

    if (m_transformDragging && event->button() == Qt::LeftButton) {
        const QPointF position =
            documentEdgePositionAtUnclamped(event->position());
        m_transformCurrentTransform = transformFromPointer(position, event->modifiers());
        m_transformDragging = false;
        const QTransform finishedTransform = m_transformCurrentTransform;
        clearTransformSnapState();
        emit transformDragFinished(finishedTransform);
        m_transformMode = CanvasTransformMode::None;
        updateTransformCursor(event->position());
        event->accept();
        return;
    }

    if (m_samplingColour && event->button() == Qt::LeftButton) {
        sampleColourAt(event->position());
        m_samplingColour = false;
        event->accept();
        return;
    }

    if (m_draggingGuide && !m_draggingNewGuide && event->button() == Qt::LeftButton) {
        finishExistingGuideDrag(event->position());
        event->accept();
        return;
    }

    if (m_panning) {
        m_panning = false;
        viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_vectorPathEditingEnabled
        && !m_spaceHeld && hasImage()) {
        emit vectorPathPointerDoubleClicked(
            documentEdgePositionAtUnclamped(event->position()), event->modifiers());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_transformDragEnabled
        && m_transformPendingChanges && !m_transformDocumentBounds.isEmpty()
        && !m_spaceHeld
        && transformModeAt(event->position()) == CanvasTransformMode::Move) {
        cancelTransformGesture();
        emit transformApplyRequested();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_cropEnabled
        && documentToViewportTransform().mapRect(m_cropFrame).contains(event->position())) {
        emit cropApplyRequested();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_selectionLassoActive
        && m_selectionLassoMode == CanvasSelectionLassoMode::Polygonal) {
        finishSelectionLassoGesture();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_guidesVisible) {
        const GuideHit hit = guideAt(event->position(), GuideHitTolerance);
        if (hit.isValid()) {
            removeGuide(hit);
            event->accept();
            return;
        }
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void ImageCanvas::leaveEvent(QEvent *event)
{
    m_mouseOverViewport = false;
    if (m_vectorPathEditingEnabled) emit vectorPathPointerLeft();
    viewport()->update();
    QAbstractScrollArea::leaveEvent(event);
}

void ImageCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Alt && !event->isAutoRepeat()
        && (m_paintMode == CanvasPaintMode::CloneStamp
            || m_paintMode == CanvasPaintMode::HealingBrush)) {
        viewport()->update();
    }
    if (event->key() == Qt::Key_Escape && !event->isAutoRepeat()
        && m_vignetteDragging) {
        m_vignetteOverlay = m_vignetteDragStartOverlay;
        m_vignetteDragging = false;
        m_vignetteDragMode = VignetteDragMode::None;
        if (m_vignetteOverlayChanged) {
            m_vignetteOverlayChanged(m_vignetteOverlay.size,
                                     m_vignetteOverlay.midpoint,
                                     m_vignetteOverlay.centreX,
                                     m_vignetteOverlay.centreY,
                                     m_vignetteOverlay.rotation);
        }
        if (m_vignetteOverlayInteractionFinished) {
            m_vignetteOverlayInteractionFinished();
        }
        updateVignetteCursor(m_lastMouseViewportPosition);
        viewport()->update();
        event->accept();
        return;
    }
    if (m_cropEnabled && !event->isAutoRepeat()) {
        if (event->key() == Qt::Key_Escape) {
            resetCropInteraction();
            emit cropCancelRequested();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            emit cropApplyRequested();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_O) {
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                ++m_cropOverlayOrientation;
            } else {
                const int count = static_cast<int>(CropOverlay::GoldenSpiral) + 1;
                m_cropOverlay = static_cast<CropOverlay>(
                    (static_cast<int>(m_cropOverlay) + 1) % count);
            }
            emit cropOverlayChanged(m_cropOverlay, m_cropOverlayOrientation);
            viewport()->update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right
            || event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
            const double amount = event->modifiers().testFlag(Qt::ShiftModifier)
                ? 10.0 : 1.0;
            QPointF delta;
            if (event->key() == Qt::Key_Left) delta.setX(-amount);
            if (event->key() == Qt::Key_Right) delta.setX(amount);
            if (event->key() == Qt::Key_Up) delta.setY(-amount);
            if (event->key() == Qt::Key_Down) delta.setY(amount);
            m_cropFrame.translate(delta);
            emit cropFrameChanged(m_cropFrame);
            emit cropFrameFinished(m_cropFrame);
            viewport()->update();
            event->accept();
            return;
        }
    }

    if (m_selectionMarqueeDragging && m_selectionMarqueeGeometryModifiersEnabled
        && !event->isAutoRepeat()
        && (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt)) {
        const QRectF previousBounds = currentSelectionMarqueeBounds();
        m_selectionMarqueeCurrentModifiers = event->modifiers();
        if (event->key() == Qt::Key_Shift)
            m_selectionMarqueeCurrentModifiers.setFlag(Qt::ShiftModifier, true);
        if (event->key() == Qt::Key_Alt)
            m_selectionMarqueeCurrentModifiers.setFlag(Qt::AltModifier, true);
        const QRectF currentBounds = currentSelectionMarqueeBounds();
        emit selectionMarqueeChanged(currentBounds);
        const QTransform toViewport = documentToViewportTransform();
        viewport()->update(toViewport.mapRect(previousBounds)
                               .united(toViewport.mapRect(currentBounds))
                               .toAlignedRect()
                               .adjusted(-5, -5, 5, 5));
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Escape && !event->isAutoRepeat()) {
        if (m_selectionLassoActive) {
            cancelSelectionLassoGesture();
            event->accept();
            return;
        }
        if (m_selectionMarqueeDragging) {
            cancelSelectionMarqueeGesture();
            event->accept();
            return;
        }
        if (m_paintMode == CanvasPaintMode::HealingBrush
            || m_paintMode == CanvasPaintMode::SpotHealing
            || m_paintMode == CanvasPaintMode::Patch) {
            // Escape cancels both an in-progress pointer gesture and the
            // asynchronous seamless solve that follows mouse release.
            cancelPaintGesture();
            emit cancelLongRunningEditRequested();
            event->accept();
            return;
        }
    }
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && !event->isAutoRepeat() && m_selectionLassoActive
        && m_selectionLassoMode == CanvasSelectionLassoMode::Polygonal) {
        finishSelectionLassoGesture();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Backspace && !event->isAutoRepeat()
        && m_selectionLassoActive
        && m_selectionLassoMode == CanvasSelectionLassoMode::Polygonal) {
        if (!m_selectionLassoPoints.isEmpty()) {
            m_selectionLassoPoints.removeLast();
        }
        if (!m_selectionLassoViewportPoints.isEmpty()) {
            m_selectionLassoViewportPoints.removeLast();
        }
        if (m_selectionLassoPoints.isEmpty()) {
            cancelSelectionLassoGesture();
        } else {
            m_selectionLassoCurrent = m_selectionLassoPoints.constLast();
            m_selectionLassoCurrentViewport =
                m_selectionLassoViewportPoints.constLast();
            emit selectionLassoChanged(currentSelectionLassoPath(true, true));
            viewport()->update();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = true;
        if (m_cropDragging && m_cropDragMode == CropDragMode::Create) {
            m_cropCreateRepositioning = true;
            m_cropCreateRepositionStartDocument = documentEdgePositionAtUnclamped(
                m_lastMouseViewportPosition);
            m_cropCreateRepositionStartFrame = m_cropFrame;
            viewport()->setCursor(Qt::SizeAllCursor);
            event->accept();
            return;
        }
        if (m_selectionMarqueeDragging) {
            m_selectionMarqueeRepositioning = true;
            m_selectionMarqueeLastRepositionPoint = m_selectionMarqueeCurrent;
            viewport()->setCursor(Qt::SizeAllCursor);
            event->accept();
            return;
        }
        if (!m_panning && !m_draggingGuide) {
            viewport()->setCursor(Qt::OpenHandCursor);
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void ImageCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (m_selectionMarqueeDragging && m_selectionMarqueeGeometryModifiersEnabled
        && !event->isAutoRepeat()
        && (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt)) {
        const QRectF previousBounds = currentSelectionMarqueeBounds();
        m_selectionMarqueeCurrentModifiers = event->modifiers();
        if (event->key() == Qt::Key_Shift)
            m_selectionMarqueeCurrentModifiers.setFlag(Qt::ShiftModifier, false);
        if (event->key() == Qt::Key_Alt)
            m_selectionMarqueeCurrentModifiers.setFlag(Qt::AltModifier, false);
        const QRectF currentBounds = currentSelectionMarqueeBounds();
        emit selectionMarqueeChanged(currentBounds);
        const QTransform toViewport = documentToViewportTransform();
        viewport()->update(toViewport.mapRect(previousBounds)
                               .united(toViewport.mapRect(currentBounds))
                               .toAlignedRect()
                               .adjusted(-5, -5, 5, 5));
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Alt && !event->isAutoRepeat()
        && (m_paintMode == CanvasPaintMode::CloneStamp
            || m_paintMode == CanvasPaintMode::HealingBrush)) {
        viewport()->update();
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        if (m_cropCreateRepositioning) {
            const QPointF documentPosition = documentEdgePositionAtUnclamped(
                m_lastMouseViewportPosition);
            const QPointF delta = documentPosition - m_cropCreateRepositionStartDocument;
            m_cropFrame = pixelAlignedCropFrame(
                m_cropCreateRepositionStartFrame.translated(delta));
            m_cropCreateAnchor += delta;
            m_cropCreateRepositioning = false;
            emit cropFrameChanged(m_cropFrame);
            viewport()->setCursor(Qt::CrossCursor);
            viewport()->update();
            event->accept();
            return;
        }
        if (m_selectionMarqueeDragging) {
            m_selectionMarqueeRepositioning = false;
            viewport()->setCursor(m_toolCursor);
            event->accept();
            return;
        }
        if (!m_panning && !m_draggingGuide) {
            viewport()->setCursor(m_toolCursor);
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::keyReleaseEvent(event);
}

void ImageCanvas::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        const QList<QUrl> urls = event->mimeData()->urls();
        const bool hasLocalFile = std::any_of(urls.cbegin(), urls.cend(), [](const QUrl &url) {
            return url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile();
        });
        if (hasLocalFile) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void ImageCanvas::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }

    bool openedAny = false;
    for (const QUrl &url : event->mimeData()->urls()) {
        const QString path = url.toLocalFile();
        if (path.isEmpty() || !QFileInfo(path).isFile()) {
            continue;
        }
        emit fileDropped(path);
        openedAny = true;
    }
    if (openedAny) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

bool ImageCanvas::sampleColourAt(const QPointF &viewportPosition)
{
    if (!m_colourSamplingEnabled || !hasImage()) {
        return false;
    }

    const std::optional<QPointF> documentPosition = documentPositionAt(viewportPosition);
    if (!documentPosition.has_value()) {
        return false;
    }

    const double normalisedX = documentPosition->x()
        / std::max(1, m_documentSize.width());
    const double normalisedY = documentPosition->y()
        / std::max(1, m_documentSize.height());
    const int imageX = std::clamp(static_cast<int>(normalisedX * m_image.width()),
                                  0,
                                  m_image.width() - 1);
    const int imageY = std::clamp(static_cast<int>(normalisedY * m_image.height()),
                                  0,
                                  m_image.height() - 1);

    emit colourSampled(m_image.pixelColor(imageX, imageY), *documentPosition);
    return true;
}

std::optional<QPointF> ImageCanvas::documentPositionAt(
    const QPointF &viewportPosition) const
{
    if (!hasImage() || !m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return std::nullopt;
    }

    const QRectF target = imageRect();
    if (!target.contains(viewportPosition) || target.width() <= 0.0
        || target.height() <= 0.0) {
        return std::nullopt;
    }

    const double normalisedX = std::clamp(
        (viewportPosition.x() - target.left()) / target.width(), 0.0, 1.0);
    const double normalisedY = std::clamp(
        (viewportPosition.y() - target.top()) / target.height(), 0.0, 1.0);
    return QPointF(normalisedX * std::max(0, m_documentSize.width() - 1),
                   normalisedY * std::max(0, m_documentSize.height() - 1));
}

QPointF ImageCanvas::documentPositionAtUnclamped(const QPointF &viewportPosition) const
{
    const QRectF target = imageRect();
    if (!hasImage() || target.width() <= 0.0 || target.height() <= 0.0
        || !m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return {};
    }
    const double normalisedX = (viewportPosition.x() - target.left()) / target.width();
    const double normalisedY = (viewportPosition.y() - target.top()) / target.height();
    return QPointF(normalisedX * std::max(0, m_documentSize.width() - 1),
                   normalisedY * std::max(0, m_documentSize.height() - 1));
}

QPointF ImageCanvas::documentEdgePositionAtUnclamped(
    const QPointF &viewportPosition) const
{
    const QRectF target = imageRect();
    if (!hasImage() || target.width() <= 0.0 || target.height() <= 0.0
        || !m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return {};
    }
    const double normalisedX = (viewportPosition.x() - target.left()) / target.width();
    const double normalisedY = (viewportPosition.y() - target.top()) / target.height();
    return QPointF(normalisedX * m_documentSize.width(),
                   normalisedY * m_documentSize.height());
}

QRectF ImageCanvas::currentSelectionMarqueeBounds() const
{
    const QPointF start = m_selectionMarqueePixelSnappingEnabled
        ? snapVectorBoundaryPoint(m_selectionMarqueeStart)
        : m_selectionMarqueeStart;
    const QPointF current = m_selectionMarqueePixelSnappingEnabled
        ? snapVectorBoundaryPoint(m_selectionMarqueeCurrent)
        : m_selectionMarqueeCurrent;
    QPointF delta = current - start;
    const bool modifierConstrained = m_selectionMarqueeGeometryModifiersEnabled
        && m_selectionMarqueeCurrentModifiers.testFlag(Qt::ShiftModifier);
    const bool modifierFromCentre = m_selectionMarqueeGeometryModifiersEnabled
        && m_selectionMarqueeCurrentModifiers.testFlag(Qt::AltModifier);
    if (m_selectionMarqueeFixedOneToOne || modifierConstrained) {
        const double side = std::max(std::abs(delta.x()), std::abs(delta.y()));
        delta.setX(std::copysign(side, qFuzzyIsNull(delta.x()) ? 1.0 : delta.x()));
        delta.setY(std::copysign(side, qFuzzyIsNull(delta.y()) ? 1.0 : delta.y()));
    }
    if (m_selectionMarqueeFromCentre || modifierFromCentre) {
        return QRectF(start - delta, start + delta).normalized();
    }
    return QRectF(start, start + delta).normalized();
}

QLineF ImageCanvas::currentSelectionMarqueeLine() const
{
    const QPointF start = m_selectionMarqueePixelSnappingEnabled
        ? snapVectorBoundaryPoint(m_selectionMarqueeStart)
        : m_selectionMarqueeStart;
    const QPointF current = m_selectionMarqueePixelSnappingEnabled
        ? snapVectorBoundaryPoint(m_selectionMarqueeCurrent)
        : m_selectionMarqueeCurrent;
    QPointF delta = current - start;
    const bool modifierConstrained = m_selectionMarqueeGeometryModifiersEnabled
        && m_selectionMarqueeCurrentModifiers.testFlag(Qt::ShiftModifier);
    if ((m_selectionMarqueeFixedOneToOne || modifierConstrained)
        && !qFuzzyIsNull(delta.x() * delta.x() + delta.y() * delta.y())) {
        // Line tools conventionally constrain to 45-degree increments rather
        // than forcing a square diagonal like rectangle/ellipse marquees.
        if (m_selectionMarqueePixelSnappingEnabled) {
            delta = constrainPixelBoundaryPointTo45(start, start + delta) - start;
        } else {
            const double length = std::hypot(delta.x(), delta.y());
            const double step = 3.14159265358979323846 / 4.0;
            const double angle = std::round(std::atan2(delta.y(), delta.x()) / step) * step;
            delta = QPointF(std::cos(angle) * length, std::sin(angle) * length);
        }
    }
    const bool fromCentre = m_selectionMarqueeFromCentre
        || (m_selectionMarqueeGeometryModifiersEnabled
            && m_selectionMarqueeCurrentModifiers.testFlag(Qt::AltModifier));
    QLineF line = fromCentre
        ? QLineF(start - delta, start + delta)
        : QLineF(start, start + delta);
    if (m_selectionMarqueePixelSnappingEnabled) {
        line.setP1(snapVectorBoundaryPoint(line.p1()));
        line.setP2(snapVectorBoundaryPoint(line.p2()));
    }
    return line;
}

bool ImageCanvas::appendFreehandLassoPoint(const QPointF &documentPosition,
                                           const QPointF &viewportPosition,
                                           const bool force)
{
    if (!m_selectionLassoActive
        || m_selectionLassoMode != CanvasSelectionLassoMode::Freehand) {
        return false;
    }
    if (!m_selectionLassoViewportPoints.isEmpty()
        && !force
        && QLineF(m_selectionLassoViewportPoints.constLast(),
                  viewportPosition).length() < FreehandLassoSampleDistance) {
        return false;
    }

    if (m_selectionLassoViewportPoints.size() >= 2) {
        const int previousIndex = m_selectionLassoViewportPoints.size() - 2;
        const QPointF previous = m_selectionLassoViewportPoints.at(previousIndex);
        const QPointF middle = m_selectionLassoViewportPoints.constLast();
        if (pointSegmentDistance(middle, previous, viewportPosition)
            <= FreehandLassoSimplifyDistance) {
            m_selectionLassoViewportPoints.last() = viewportPosition;
            m_selectionLassoPoints.last() = documentPosition;
            m_selectionLassoCurrent = documentPosition;
            m_selectionLassoCurrentViewport = viewportPosition;
            return true;
        }
    }

    m_selectionLassoViewportPoints.push_back(viewportPosition);
    m_selectionLassoPoints.push_back(documentPosition);
    m_selectionLassoCurrent = documentPosition;
    m_selectionLassoCurrentViewport = viewportPosition;
    return true;
}

QPainterPath ImageCanvas::currentSelectionLassoPath(const bool includeLivePoint,
                                                    const bool closePath) const
{
    QPolygonF points = m_selectionLassoPoints;
    if (includeLivePoint && m_selectionLassoMode == CanvasSelectionLassoMode::Polygonal
        && m_selectionLassoActive && !points.isEmpty()
        && (points.constLast() != m_selectionLassoCurrent)) {
        points.push_back(m_selectionLassoCurrent);
    }
    if (points.size() < 2) {
        return {};
    }

    QPainterPath path(points.constFirst());
    for (int index = 1; index < points.size(); ++index) {
        path.lineTo(points.at(index));
    }
    if (closePath && points.size() >= 3) {
        path.closeSubpath();
    }
    path.setFillRule(Qt::OddEvenFill);
    return path;
}

QRect ImageCanvas::selectionLassoViewportBounds() const
{
    if (!m_selectionLassoActive || m_selectionLassoPoints.isEmpty()) {
        return {};
    }
    const QPainterPath documentPath = currentSelectionLassoPath(true, true);
    QRectF bounds = documentToViewportTransform().map(documentPath).boundingRect();
    if (m_selectionLassoMode == CanvasSelectionLassoMode::Polygonal
        && !m_selectionLassoViewportPoints.isEmpty()) {
        bounds = bounds.united(QRectF(m_selectionLassoViewportPoints.constFirst()
                                         - QPointF(7.0, 7.0),
                                     QSizeF(14.0, 14.0)));
    }
    return bounds.toAlignedRect().adjusted(-5, -5, 5, 5);
}

void ImageCanvas::finishSelectionLassoGesture()
{
    if (!m_selectionLassoActive) {
        return;
    }

    // Remove an accidental duplicate closing sample while preserving the
    // original first point. The raster path is closed explicitly below.
    while (m_selectionLassoPoints.size() >= 2
           && QLineF(m_selectionLassoViewportPoints.constFirst(),
                     m_selectionLassoViewportPoints.constLast()).length() < 0.5) {
        m_selectionLassoPoints.removeLast();
        m_selectionLassoViewportPoints.removeLast();
    }

    const QPainterPath path = currentSelectionLassoPath(false, true);
    const bool valid = m_selectionLassoPoints.size() >= 3
        && !path.isEmpty() && path.boundingRect().width() > 0.0
        && path.boundingRect().height() > 0.0;

    m_selectionLassoActive = false;
    m_selectionLassoPointerDown = false;
    m_selectionLassoPoints.clear();
    m_selectionLassoViewportPoints.clear();
    m_selectionLassoCurrent = {};
    m_selectionLassoCurrentViewport = {};

    if (valid) {
        emit selectionLassoFinished(path);
    } else {
        emit selectionLassoCancelled();
    }
    viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
    viewport()->update();
}

QPointF ImageCanvas::previewPositionForDocument(const QPointF &documentPosition) const
{
    if (m_image.isNull() || !m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return {};
    }
    return QPointF(documentPosition.x() * std::max(0, m_image.width() - 1)
                       / std::max(1, m_documentSize.width() - 1),
                   documentPosition.y() * std::max(0, m_image.height() - 1)
                       / std::max(1, m_documentSize.height() - 1));
}

QRect ImageCanvas::viewportRectForPreviewRegion(const QRect &previewRegion) const
{
    if (m_image.isNull() || previewRegion.isEmpty()) {
        return {};
    }
    const QRectF target = imageRect();
    const double scaleX = target.width() / std::max(1, m_image.width());
    const double scaleY = target.height() / std::max(1, m_image.height());
    return QRectF(target.left() + previewRegion.left() * scaleX - 3.0,
                  target.top() + previewRegion.top() * scaleY - 3.0,
                  previewRegion.width() * scaleX + 6.0,
                  previewRegion.height() * scaleY + 6.0)
        .toAlignedRect();
}

void ImageCanvas::notifyPresentationViewportChanged(const bool settled)
{
    if (!hasImage()) {
        return;
    }
    const QRect visible = visiblePreviewRegion();
    if (!visible.isEmpty()) {
        emit presentationViewportChanged(visible, m_zoom, settled);
    }
    if (!settled) {
        m_presentationSettleTimer.start();
    }
}

void ImageCanvas::trimPresentationTiles()
{
    constexpr int MaximumPresentedTiles = 384;
    while (m_presentationTiles.size() > MaximumPresentedTiles) {
        auto oldest = m_presentationTiles.begin();
        for (auto it = m_presentationTiles.begin(); it != m_presentationTiles.end(); ++it) {
            if (it->lastUseSerial < oldest->lastUseSerial
                || (it->lastUseSerial == oldest->lastUseSerial
                    && it.key() < oldest.key())) {
                oldest = it;
            }
        }
        m_presentationTiles.erase(oldest);
    }
}

QString ImageCanvas::presentationTileKey(const QRect &basePreviewRect, const int level)
{
    return QStringLiteral("%1/%2/%3/%4/%5")
        .arg(std::max(0, level))
        .arg(basePreviewRect.x())
        .arg(basePreviewRect.y())
        .arg(basePreviewRect.width())
        .arg(basePreviewRect.height());
}

QRectF ImageCanvas::imageRect() const
{
    if (!hasImage()) {
        return {};
    }

    const QSizeF scaledSize(m_image.width() * m_zoom, m_image.height() * m_zoom);
    if (m_fitMode) {
        return QRectF(QPointF((viewport()->width() - scaledSize.width()) * 0.5,
                              (viewport()->height() - scaledSize.height()) * 0.5),
                      scaledSize);
    }

    // Treat the free canvas as if it had a half-viewport gutter around the
    // image. At either end of the scroll range, the corresponding image edge
    // therefore sits at the centre of the visible canvas rather than being
    // trapped against the viewport edge.
    const double x = viewport()->width() * 0.5 - horizontalScrollBar()->value();
    const double y = viewport()->height() * 0.5 - verticalScrollBar()->value();
    return QRectF(QPointF(x, y), scaledSize);
}



ImageCanvas::CropDragMode ImageCanvas::cropModeAt(const QPointF &viewportPosition) const
{
    if (!m_cropEnabled || m_cropFrame.isEmpty()) {
        return CropDragMode::None;
    }
    const QRectF frame = documentToViewportTransform().mapRect(m_cropFrame).normalized();
    constexpr double handle = 8.0;
    const bool nearLeft = std::abs(viewportPosition.x() - frame.left()) <= handle;
    const bool nearRight = std::abs(viewportPosition.x() - frame.right()) <= handle;
    const bool nearTop = std::abs(viewportPosition.y() - frame.top()) <= handle;
    const bool nearBottom = std::abs(viewportPosition.y() - frame.bottom()) <= handle;
    const bool withinX = viewportPosition.x() >= frame.left() - handle
        && viewportPosition.x() <= frame.right() + handle;
    const bool withinY = viewportPosition.y() >= frame.top() - handle
        && viewportPosition.y() <= frame.bottom() + handle;

    if (m_cropConstraintMode != CropMode::FixedSize) {
        if (nearLeft && nearTop) return CropDragMode::TopLeft;
        if (nearRight && nearTop) return CropDragMode::TopRight;
        if (nearRight && nearBottom) return CropDragMode::BottomRight;
        if (nearLeft && nearBottom) return CropDragMode::BottomLeft;
        if (nearLeft && withinY) return CropDragMode::Left;
        if (nearRight && withinY) return CropDragMode::Right;
        if (nearTop && withinX) return CropDragMode::Top;
        if (nearBottom && withinX) return CropDragMode::Bottom;
    }
    return frame.contains(viewportPosition) ? CropDragMode::Move : CropDragMode::Create;
}

QVector<double> ImageCanvas::cropSnapTargets(const Qt::Orientation orientation) const
{
    QVector<double> targets;
    if (orientation == Qt::Vertical) {
        targets = {0.0, m_documentSize.width() * 0.5,
                   static_cast<double>(m_documentSize.width())};
        targets += m_verticalGuides;
        for (const QRectF &bounds : m_cropSnapBounds) {
            targets << bounds.left() << bounds.center().x() << bounds.right();
        }
    } else {
        targets = {0.0, m_documentSize.height() * 0.5,
                   static_cast<double>(m_documentSize.height())};
        targets += m_horizontalGuides;
        for (const QRectF &bounds : m_cropSnapBounds) {
            targets << bounds.top() << bounds.center().y() << bounds.bottom();
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end(), [](double left, double right) {
        return std::abs(left - right) < 1.0e-6;
    }), targets.end());
    return targets;
}

QRectF ImageCanvas::snappedCropFrame(const QRectF &candidate,
                                     const CropDragMode mode) const
{
    if (!m_cropSnappingEnabled || candidate.isEmpty()) {
        return candidate;
    }
    QRectF result = candidate;
    const double toleranceX = 8.0 / std::max(1.0e-6, documentScale(Qt::Vertical));
    const double toleranceY = 8.0 / std::max(1.0e-6, documentScale(Qt::Horizontal));
    const auto snap = [](const QVector<double> &anchors,
                         const QVector<double> &targets,
                         const double tolerance,
                         double *correction) {
        double best = tolerance + 1.0;
        double bestCorrection = 0.0;
        for (const double anchor : anchors) {
            for (const double target : targets) {
                const double distance = std::abs(target - anchor);
                if (distance < best && distance <= tolerance) {
                    best = distance;
                    bestCorrection = target - anchor;
                }
            }
        }
        if (best <= tolerance) {
            *correction = bestCorrection;
            return true;
        }
        return false;
    };

    double correction = 0.0;
    if (mode == CropDragMode::Move || mode == CropDragMode::Create) {
        if (snap({result.left(), result.center().x(), result.right()},
                 cropSnapTargets(Qt::Vertical), toleranceX, &correction)) {
            result.translate(correction, 0.0);
        }
        if (snap({result.top(), result.center().y(), result.bottom()},
                 cropSnapTargets(Qt::Horizontal), toleranceY, &correction)) {
            result.translate(0.0, correction);
        }
        return result;
    }

    const bool changesLeft = mode == CropDragMode::Left
        || mode == CropDragMode::TopLeft || mode == CropDragMode::BottomLeft;
    const bool changesRight = mode == CropDragMode::Right
        || mode == CropDragMode::TopRight || mode == CropDragMode::BottomRight;
    const bool changesTop = mode == CropDragMode::Top
        || mode == CropDragMode::TopLeft || mode == CropDragMode::TopRight;
    const bool changesBottom = mode == CropDragMode::Bottom
        || mode == CropDragMode::BottomLeft || mode == CropDragMode::BottomRight;
    if (changesLeft && snap({result.left()}, cropSnapTargets(Qt::Vertical),
                            toleranceX, &correction)) {
        result.setLeft(result.left() + correction);
    } else if (changesRight && snap({result.right()}, cropSnapTargets(Qt::Vertical),
                                    toleranceX, &correction)) {
        result.setRight(result.right() + correction);
    }
    if (changesTop && snap({result.top()}, cropSnapTargets(Qt::Horizontal),
                           toleranceY, &correction)) {
        result.setTop(result.top() + correction);
    } else if (changesBottom && snap({result.bottom()}, cropSnapTargets(Qt::Horizontal),
                                     toleranceY, &correction)) {
        result.setBottom(result.bottom() + correction);
    }
    return result.normalized();
}

QRectF ImageCanvas::constrainedCropFrame(const QRectF &candidate,
                                         const CropDragMode mode,
                                         const Qt::KeyboardModifiers modifiers) const
{
    QRectF result = candidate.normalized();
    double ratio = 0.0;
    if (m_cropConstraintMode == CropMode::Ratio) {
        ratio = m_cropAspectRatio;
    } else if (m_cropConstraintMode == CropMode::Free
               && modifiers.testFlag(Qt::ShiftModifier)
               && m_cropDragStartFrame.height() > 0.0) {
        ratio = m_cropDragStartFrame.width() / m_cropDragStartFrame.height();
    }
    if (m_cropConstraintMode == CropMode::FixedSize
        && m_cropFixedSize.width() > 0 && m_cropFixedSize.height() > 0) {
        result.setSize(m_cropFixedSize);
        return result;
    }
    if (ratio <= 0.0 || result.isEmpty()) {
        result.setWidth(std::max(1.0, result.width()));
        result.setHeight(std::max(1.0, result.height()));
        return result;
    }

    const QRectF start = m_cropDragStartFrame;
    const bool horizontalEdge = mode == CropDragMode::Left || mode == CropDragMode::Right;
    const bool verticalEdge = mode == CropDragMode::Top || mode == CropDragMode::Bottom;
    if (verticalEdge || (!horizontalEdge && result.width() / std::max(1.0, result.height()) > ratio)) {
        const double wantedWidth = std::max(1.0, result.height() * ratio);
        if (mode == CropDragMode::TopLeft || mode == CropDragMode::BottomLeft) {
            result.setLeft(result.right() - wantedWidth);
        } else if (mode == CropDragMode::Left) {
            result.setLeft(result.right() - wantedWidth);
        } else {
            result.setRight(result.left() + wantedWidth);
        }
    } else {
        const double wantedHeight = std::max(1.0, result.width() / ratio);
        if (mode == CropDragMode::TopLeft || mode == CropDragMode::TopRight
            || mode == CropDragMode::Top) {
            result.setTop(result.bottom() - wantedHeight);
        } else {
            result.setBottom(result.top() + wantedHeight);
        }
    }

    if (modifiers.testFlag(Qt::AltModifier)) {
        const QPointF centre = start.center();
        result.moveCenter(centre);
    }
    result.setWidth(std::max(1.0, result.width()));
    result.setHeight(std::max(1.0, result.height()));
    return result.normalized();
}

QRectF ImageCanvas::cropFrameFromPointer(const QPointF &documentPosition,
                                         const Qt::KeyboardModifiers modifiers) const
{
    QRectF result = m_cropDragStartFrame;
    const QPointF delta = documentPosition - m_cropDragStartDocument;
    if (m_cropDragMode == CropDragMode::Move) {
        result.translate(delta);
        return pixelAlignedCropFrame(snappedCropFrame(result, m_cropDragMode));
    }
    if (m_cropDragMode == CropDragMode::Create) {
        QPointF anchor = m_cropCreateAnchor;
        QPointF current = documentPosition;
        if (modifiers.testFlag(Qt::AltModifier)) {
            const QPointF extent = current - anchor;
            result = QRectF(anchor - extent, anchor + extent).normalized();
        } else {
            result = QRectF(anchor, current).normalized();
        }
        result = constrainedCropFrame(result, m_cropDragMode, modifiers);
        return pixelAlignedCropFrame(snappedCropFrame(result, m_cropDragMode));
    }

    if (m_cropDragMode == CropDragMode::Left
        || m_cropDragMode == CropDragMode::TopLeft
        || m_cropDragMode == CropDragMode::BottomLeft) {
        result.setLeft(documentPosition.x());
    }
    if (m_cropDragMode == CropDragMode::Right
        || m_cropDragMode == CropDragMode::TopRight
        || m_cropDragMode == CropDragMode::BottomRight) {
        result.setRight(documentPosition.x());
    }
    if (m_cropDragMode == CropDragMode::Top
        || m_cropDragMode == CropDragMode::TopLeft
        || m_cropDragMode == CropDragMode::TopRight) {
        result.setTop(documentPosition.y());
    }
    if (m_cropDragMode == CropDragMode::Bottom
        || m_cropDragMode == CropDragMode::BottomLeft
        || m_cropDragMode == CropDragMode::BottomRight) {
        result.setBottom(documentPosition.y());
    }
    if (modifiers.testFlag(Qt::AltModifier)) {
        const QPointF centre = m_cropDragStartFrame.center();
        if (m_cropDragMode == CropDragMode::Left || m_cropDragMode == CropDragMode::Right
            || m_cropDragMode == CropDragMode::TopLeft || m_cropDragMode == CropDragMode::TopRight
            || m_cropDragMode == CropDragMode::BottomLeft || m_cropDragMode == CropDragMode::BottomRight) {
            const double halfWidth = std::abs(documentPosition.x() - centre.x());
            result.setLeft(centre.x() - halfWidth);
            result.setRight(centre.x() + halfWidth);
        }
        if (m_cropDragMode == CropDragMode::Top || m_cropDragMode == CropDragMode::Bottom
            || m_cropDragMode == CropDragMode::TopLeft || m_cropDragMode == CropDragMode::TopRight
            || m_cropDragMode == CropDragMode::BottomLeft || m_cropDragMode == CropDragMode::BottomRight) {
            const double halfHeight = std::abs(documentPosition.y() - centre.y());
            result.setTop(centre.y() - halfHeight);
            result.setBottom(centre.y() + halfHeight);
        }
    }
    result = constrainedCropFrame(result, m_cropDragMode, modifiers);
    return pixelAlignedCropFrame(snappedCropFrame(result, m_cropDragMode));
}

void ImageCanvas::updateCropCursor(const QPointF &viewportPosition)
{
    if (!m_cropEnabled || m_spaceHeld) {
        return;
    }
    if (m_cropStraightenSampling) {
        viewport()->setCursor(Qt::CrossCursor);
        return;
    }
    switch (cropModeAt(viewportPosition)) {
    case CropDragMode::Move: viewport()->setCursor(Qt::SizeAllCursor); break;
    case CropDragMode::Left:
    case CropDragMode::Right: viewport()->setCursor(Qt::SizeHorCursor); break;
    case CropDragMode::Top:
    case CropDragMode::Bottom: viewport()->setCursor(Qt::SizeVerCursor); break;
    case CropDragMode::TopLeft:
    case CropDragMode::BottomRight: viewport()->setCursor(Qt::SizeFDiagCursor); break;
    case CropDragMode::TopRight:
    case CropDragMode::BottomLeft: viewport()->setCursor(Qt::SizeBDiagCursor); break;
    case CropDragMode::Create:
    case CropDragMode::None: viewport()->setCursor(Qt::CrossCursor); break;
    }
}

void ImageCanvas::paintCropOverlay(QPainter &painter) const
{
    if (!m_cropEnabled || m_cropFrame.isEmpty()) {
        return;
    }
    const QRectF frame = documentToViewportTransform().mapRect(m_cropFrame).normalized();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath outside;
    outside.addRect(viewport()->rect());
    QPainterPath inside;
    inside.addRect(frame);
    outside = outside.subtracted(inside);
    painter.fillPath(outside, QColor(0, 0, 0,
        qRound(std::clamp(m_cropDimOpacity, 0.0, 0.95) * 255.0)));

    painter.setClipRect(frame);
    painter.setPen(QPen(QColor(255, 255, 255, 190), 1.0));
    const auto line = [&painter](const QPointF &a, const QPointF &b) {
        painter.drawLine(QLineF(a, b));
    };
    const double left = frame.left();
    const double right = frame.right();
    const double top = frame.top();
    const double bottom = frame.bottom();
    const double width = frame.width();
    const double height = frame.height();
    if (m_cropOverlay == CropOverlay::RuleOfThirds) {
        line({left + width / 3.0, top}, {left + width / 3.0, bottom});
        line({left + width * 2.0 / 3.0, top}, {left + width * 2.0 / 3.0, bottom});
        line({left, top + height / 3.0}, {right, top + height / 3.0});
        line({left, top + height * 2.0 / 3.0}, {right, top + height * 2.0 / 3.0});
    } else if (m_cropOverlay == CropOverlay::Grid) {
        for (int i = 1; i < 4; ++i) {
            line({left + width * i / 4.0, top}, {left + width * i / 4.0, bottom});
            line({left, top + height * i / 4.0}, {right, top + height * i / 4.0});
        }
    } else if (m_cropOverlay == CropOverlay::Diagonal) {
        line(frame.topLeft(), frame.bottomRight());
        line(frame.topRight(), frame.bottomLeft());
    } else if (m_cropOverlay == CropOverlay::Triangle) {
        const bool reverse = (m_cropOverlayOrientation & 1) != 0;
        line(reverse ? frame.topRight() : frame.topLeft(),
             reverse ? frame.bottomLeft() : frame.bottomRight());
        const QPointF midpoint = reverse
            ? QPointF(left + width * 0.5, bottom)
            : QPointF(left + width * 0.5, top);
        line(reverse ? frame.topLeft() : frame.topRight(), midpoint);
        line(reverse ? frame.bottomRight() : frame.bottomLeft(), midpoint);
    } else if (m_cropOverlay == CropOverlay::GoldenRatio
               || m_cropOverlay == CropOverlay::GoldenSpiral) {
        constexpr double phi = 0.6180339887498948;
        line({left + width * phi, top}, {left + width * phi, bottom});
        line({left, top + height * phi}, {right, top + height * phi});
        if (m_cropOverlay == CropOverlay::GoldenSpiral) {
            QRectF spiral = frame.adjusted(width * 0.06, height * 0.06,
                                           -width * 0.06, -height * 0.06);
            QPainterPath path;
            const bool mirror = (m_cropOverlayOrientation & 1) != 0;
            path.moveTo(mirror ? spiral.topRight() : spiral.topLeft());
            path.cubicTo(spiral.center(),
                         mirror ? spiral.bottomLeft() : spiral.bottomRight(),
                         spiral.center());
            path.cubicTo(mirror ? spiral.topRight() : spiral.topLeft(),
                         mirror ? spiral.bottomRight() : spiral.bottomLeft(),
                         QPointF(spiral.center().x(), spiral.top() + spiral.height() * 0.42));
            painter.drawPath(path);
        }
    }
    painter.setClipping(false);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 230), 3.0));
    painter.drawRect(frame);
    painter.setPen(QPen(QColor(QStringLiteral("#f4b860")), 1.0));
    painter.drawRect(frame);

    const QVector<QPointF> handles {
        frame.topLeft(), QPointF(frame.center().x(), frame.top()), frame.topRight(),
        QPointF(frame.right(), frame.center().y()), frame.bottomRight(),
        QPointF(frame.center().x(), frame.bottom()), frame.bottomLeft(),
        QPointF(frame.left(), frame.center().y())
    };
    if (m_cropConstraintMode != CropMode::FixedSize) {
        for (const QPointF &point : handles) {
            const QRectF box(point - QPointF(4.5, 4.5), QSizeF(9.0, 9.0));
            painter.fillRect(box, QColor(QStringLiteral("#f4b860")));
            painter.setPen(QPen(themeColour(QStringLiteral("preview_background")), 1.0));
            painter.drawRect(box);
        }
    }

    if (m_cropStraightenDragging) {
        const QTransform transform = documentToViewportTransform();
        const QLineF line(transform.map(m_cropStraightenStart),
                          transform.map(m_cropStraightenCurrent));
        painter.setPen(QPen(QColor(0, 0, 0, 230), 3.0));
        painter.drawLine(line);
        painter.setPen(QPen(QColor(QStringLiteral("#ffd166")), 1.0));
        painter.drawLine(line);
    }
    painter.restore();
}

QTransform ImageCanvas::documentToViewportTransform() const
{
    const QRectF target = imageRect();
    if (target.isEmpty() || m_documentSize.isEmpty()) {
        return {};
    }
    return QTransform::fromScale(
               target.width() / static_cast<double>(m_documentSize.width()),
               target.height() / static_cast<double>(m_documentSize.height()))
        * QTransform::fromTranslate(target.left(), target.top());
}

QPolygonF ImageCanvas::transformSourceQuad() const
{
    if (m_transformDocumentBounds.isEmpty()) {
        return {};
    }
    QPolygonF quad;
    quad << m_transformDocumentBounds.topLeft()
         << m_transformDocumentBounds.topRight()
         << m_transformDocumentBounds.bottomRight()
         << m_transformDocumentBounds.bottomLeft();
    return quad;
}

QPolygonF ImageCanvas::transformDocumentQuad(const QTransform &transform) const
{
    return transform.map(transformSourceQuad());
}

bool ImageCanvas::transformFromQuad(const QPolygonF &source,
                                    const QPolygonF &target,
                                    QTransform *result)
{
    if (!result || !isUsableTransformQuad(source)
        || !isUsableTransformQuad(target)) {
        return false;
    }
    QTransform candidate;
    if (!QTransform::quadToQuad(source, target, candidate)
        || !candidate.isInvertible()
        || !transformHasContinuousProjectiveDomain(candidate, source)) {
        return false;
    }
    const double values[] = {
        candidate.m11(), candidate.m12(), candidate.m13(),
        candidate.m21(), candidate.m22(), candidate.m23(),
        candidate.m31(), candidate.m32(), candidate.m33(),
    };
    for (const double value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    *result = candidate;
    return true;
}

int ImageCanvas::transformControlPointIndex(const CanvasTransformMode mode)
{
    switch (mode) {
    case CanvasTransformMode::SkewTop:
    case CanvasTransformMode::ControlTopLeft:
        return 0;
    case CanvasTransformMode::SkewRight:
    case CanvasTransformMode::ControlTopRight:
        return 1;
    case CanvasTransformMode::SkewBottom:
    case CanvasTransformMode::ControlBottomRight:
        return 2;
    case CanvasTransformMode::SkewLeft:
    case CanvasTransformMode::ControlBottomLeft:
        return 3;
    default:
        return -1;
    }
}

QPolygonF ImageCanvas::transformBoxPolygon(const QTransform &documentTransform) const
{
    return documentToViewportTransform().map(transformDocumentQuad(documentTransform));
}

QVector<QPointF> ImageCanvas::transformHandlePoints(const QPolygonF &box) const
{
    if (box.size() != 4) {
        return {};
    }
    const QPointF topLeft = box.at(0);
    const QPointF topRight = box.at(1);
    const QPointF bottomRight = box.at(2);
    const QPointF bottomLeft = box.at(3);
    const QPointF top = (topLeft + topRight) * 0.5;
    const QPointF right = (topRight + bottomRight) * 0.5;
    const QPointF bottom = (bottomRight + bottomLeft) * 0.5;
    const QPointF left = (bottomLeft + topLeft) * 0.5;

    const QPointF edge = topRight - topLeft;
    const double edgeLength = std::hypot(edge.x(), edge.y());
    QPointF outward(0.0, -1.0);
    if (edgeLength > 1.0e-6) {
        outward = QPointF(edge.y() / edgeLength, -edge.x() / edgeLength);
    }
    const QPointF rotation = top + outward * 28.0;
    return {topLeft, top, topRight, right, bottomRight, bottom, bottomLeft, left, rotation};
}

CanvasTransformMode ImageCanvas::transformModeAt(const QPointF &viewportPosition) const
{
    if (!m_transformDragEnabled || m_transformDocumentBounds.isEmpty()) {
        return CanvasTransformMode::None;
    }
    const QPolygonF box = transformBoxPolygon(m_transformCurrentTransform);
    const QVector<QPointF> handles = transformHandlePoints(box);
    if (handles.size() != 9) {
        return CanvasTransformMode::None;
    }

    const bool affineMode = m_transformInteractionMode
            == CanvasTransformInteractionMode::FreeTransform
        || m_transformInteractionMode == CanvasTransformInteractionMode::Scale
        || m_transformInteractionMode == CanvasTransformInteractionMode::Rotate;
    if (affineMode) {
        const QPointF pivotViewport = documentToViewportTransform().map(transformPivot());
        if (QLineF(viewportPosition, pivotViewport).length() <= 9.0) {
            return CanvasTransformMode::Pivot;
        }
    }

    if (m_transformInteractionMode == CanvasTransformInteractionMode::Skew) {
        static constexpr int handleIndices[] = {1, 3, 5, 7};
        static constexpr CanvasTransformMode modes[] = {
            CanvasTransformMode::SkewTop,
            CanvasTransformMode::SkewRight,
            CanvasTransformMode::SkewBottom,
            CanvasTransformMode::SkewLeft,
        };
        for (int index = 0; index < 4; ++index) {
            if (QRectF(handles.at(handleIndices[index]) - QPointF(8.0, 8.0),
                       QSizeF(16.0, 16.0)).contains(viewportPosition)) {
                return modes[index];
            }
        }
    } else if (m_transformInteractionMode == CanvasTransformInteractionMode::Distort
               || m_transformInteractionMode
                      == CanvasTransformInteractionMode::Perspective) {
        static constexpr int handleIndices[] = {0, 2, 4, 6};
        static constexpr CanvasTransformMode modes[] = {
            CanvasTransformMode::ControlTopLeft,
            CanvasTransformMode::ControlTopRight,
            CanvasTransformMode::ControlBottomRight,
            CanvasTransformMode::ControlBottomLeft,
        };
        for (int index = 0; index < 4; ++index) {
            if (QRectF(handles.at(handleIndices[index]) - QPointF(8.0, 8.0),
                       QSizeF(16.0, 16.0)).contains(viewportPosition)) {
                return modes[index];
            }
        }
    } else {
        if (m_transformInteractionMode != CanvasTransformInteractionMode::Scale
            && QLineF(viewportPosition, handles.at(8)).length() <= 9.0) {
            return CanvasTransformMode::Rotate;
        }
        static constexpr CanvasTransformMode modes[] = {
            CanvasTransformMode::ScaleTopLeft,
            CanvasTransformMode::ScaleTop,
            CanvasTransformMode::ScaleTopRight,
            CanvasTransformMode::ScaleRight,
            CanvasTransformMode::ScaleBottomRight,
            CanvasTransformMode::ScaleBottom,
            CanvasTransformMode::ScaleBottomLeft,
            CanvasTransformMode::ScaleLeft
        };
        if (m_transformInteractionMode != CanvasTransformInteractionMode::Rotate) {
            for (int index = 0; index < 8; ++index) {
                if (QRectF(handles.at(index) - QPointF(7.0, 7.0), QSizeF(14.0, 14.0))
                        .contains(viewportPosition)) {
                    return modes[index];
                }
            }
        }
    }
    return box.containsPoint(viewportPosition, Qt::OddEvenFill)
        ? CanvasTransformMode::Move
        : CanvasTransformMode::None;
}

QVector<double> ImageCanvas::transformSnapTargets(const Qt::Orientation orientation) const
{
    QVector<double> targets;
    if (!m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return targets;
    }

    const double maximum = orientation == Qt::Vertical
        ? static_cast<double>(m_documentSize.width())
        : static_cast<double>(m_documentSize.height());
    targets.reserve(3 + (orientation == Qt::Vertical
                             ? m_verticalGuides.size()
                             : m_horizontalGuides.size())
                    + m_transformSnapBounds.size() * 3
                    + m_transformSnapTargetPoints.size());
    targets << 0.0 << maximum * 0.5 << maximum;

    if (m_guidesVisible) {
        const QVector<double> &guides = orientation == Qt::Vertical
            ? m_verticalGuides
            : m_horizontalGuides;
        for (const double guide : guides) {
            targets.push_back(guide);
        }
    }

    for (const QRectF &bounds : m_transformSnapBounds) {
        if (orientation == Qt::Vertical) {
            targets << bounds.left() << bounds.center().x() << bounds.right();
        } else {
            targets << bounds.top() << bounds.center().y() << bounds.bottom();
        }
    }
    for (const QPointF &point : m_transformSnapTargetPoints) {
        targets.push_back(orientation == Qt::Vertical ? point.x() : point.y());
    }

    sortAndDeduplicate(targets);
    return targets;
}

ImageCanvas::TransformAxisSnap ImageCanvas::resolveTransformAxisSnap(
    const Qt::Orientation orientation,
    const QVector<double> &anchors,
    const bool requireWholePixelCorrection,
    const bool requireWholePixelTarget)
{
    bool &active = orientation == Qt::Vertical
        ? m_transformSnapXActive
        : m_transformSnapYActive;
    double &activeTarget = orientation == Qt::Vertical
        ? m_transformSnapXTarget
        : m_transformSnapYTarget;
    int &activeAnchor = orientation == Qt::Vertical
        ? m_transformSnapXAnchor
        : m_transformSnapYAnchor;

    TransformAxisSnap result;
    if (anchors.isEmpty() || !m_transformSnappingEnabled) {
        active = false;
        activeTarget = 0.0;
        activeAnchor = -1;
        return result;
    }

    const double scale = std::max(1.0e-9, documentScale(orientation));
    const double releaseDistance = m_transformSnapDistance + TransformSnapReleasePadding;
    if (active && activeAnchor >= 0 && activeAnchor < anchors.size()) {
        const double correction = activeTarget - anchors.at(activeAnchor);
        const bool targetAllowed = !requireWholePixelTarget
            || std::abs(activeTarget - std::round(activeTarget)) <= 1.0e-9;
        const bool correctionAllowed = !requireWholePixelCorrection
            || std::abs(correction - std::round(correction)) <= 1.0e-9;
        const double distance = std::abs(correction) * scale;
        if (targetAllowed && correctionAllowed && distance <= releaseDistance) {
            result.active = true;
            result.target = activeTarget;
            result.anchorIndex = activeAnchor;
            result.screenDistance = distance;
            result.correction = correction;
            return result;
        }
    }

    active = false;
    activeTarget = 0.0;
    activeAnchor = -1;

    const QVector<double> targets = transformSnapTargets(orientation);
    double bestDistance = std::numeric_limits<double>::max();
    int bestAnchor = -1;
    double bestTarget = 0.0;
    for (int anchorIndex = 0; anchorIndex < anchors.size(); ++anchorIndex) {
        const double anchor = anchors.at(anchorIndex);
        for (const double target : targets) {
            const double correction = target - anchor;
            if (requireWholePixelTarget
                && std::abs(target - std::round(target)) > 1.0e-9) {
                continue;
            }
            if (requireWholePixelCorrection
                && std::abs(correction - std::round(correction)) > 1.0e-9) {
                continue;
            }
            const double distance = std::abs(correction) * scale;
            if (distance <= m_transformSnapDistance && distance < bestDistance) {
                bestDistance = distance;
                bestAnchor = anchorIndex;
                bestTarget = target;
            }
        }
    }

    if (bestAnchor < 0) {
        return result;
    }

    active = true;
    activeTarget = bestTarget;
    activeAnchor = bestAnchor;
    result.active = true;
    result.target = bestTarget;
    result.anchorIndex = bestAnchor;
    result.screenDistance = bestDistance;
    result.correction = bestTarget - anchors.at(bestAnchor);
    return result;
}

void ImageCanvas::clearTransformSnapState()
{
    m_transformSnapXActive = false;
    m_transformSnapYActive = false;
    m_transformSnapXTarget = 0.0;
    m_transformSnapYTarget = 0.0;
    m_transformSnapXAnchor = -1;
    m_transformSnapYAnchor = -1;
}

QTransform ImageCanvas::transformFromPointer(
    const QPointF &documentPosition,
    const Qt::KeyboardModifiers modifiers)
{
    const bool snapping = m_transformSnappingEnabled
        && !modifiers.testFlag(Qt::ControlModifier);
    const QTransform base = m_transformGestureBaseTransform;

    if (m_transformMode == CanvasTransformMode::Move) {
        QPointF delta = documentPosition - m_transformStartDocumentPosition;
        if (snapping) {
            // Quantise the resulting layer bounds, not merely the pointer delta.
            // Older projects can contain fractional transforms; rounding only
            // delta would preserve that fractional offset forever. Anchoring the
            // current transformed top-left boundary to the integer lattice lets
            // raster and axis-aligned vector bounds recover clean pixel edges.
            const QRectF baseBounds = base
                .mapRect(m_transformDocumentBounds).normalized();
            delta.setX(std::round(baseBounds.left() + delta.x())
                       - baseBounds.left());
            delta.setY(std::round(baseBounds.top() + delta.y())
                       - baseBounds.top());
        }
        QTransform transform = base
            * QTransform::fromTranslate(delta.x(), delta.y());
        if (!snapping) {
            clearTransformSnapState();
            m_transformPivotDocument = m_transformGestureStartPivotDocument + delta;
            return transform;
        }

        const QRectF movedBounds = transform.mapRect(m_transformDocumentBounds);
        QVector<double> xAnchors {movedBounds.left(), movedBounds.center().x(),
                                  movedBounds.right()};
        QVector<double> yAnchors {movedBounds.top(), movedBounds.center().y(),
                                  movedBounds.bottom()};
        xAnchors.reserve(xAnchors.size() + m_transformSnapSourcePoints.size());
        yAnchors.reserve(yAnchors.size() + m_transformSnapSourcePoints.size());
        for (const QPointF &point : m_transformSnapSourcePoints) {
            const QPointF moved = transform.map(point);
            xAnchors.push_back(moved.x());
            yAnchors.push_back(moved.y());
        }
        const TransformAxisSnap xSnap = resolveTransformAxisSnap(
            Qt::Vertical, xAnchors, true, false);
        const TransformAxisSnap ySnap = resolveTransformAxisSnap(
            Qt::Horizontal, yAnchors, true, false);
        const QPointF correction(xSnap.correction, ySnap.correction);
        m_transformPivotDocument = m_transformGestureStartPivotDocument
            + delta + correction;
        return transform * QTransform::fromTranslate(correction.x(), correction.y());
    }

    if (m_transformMode == CanvasTransformMode::Rotate) {
        clearTransformSnapState();
        const QPointF center = m_transformGestureStartPivotDocument;
        const QPointF startVector = m_transformStartDocumentPosition - center;
        const QPointF currentVector = documentPosition - center;
        const double startAngle = std::atan2(startVector.y(), startVector.x());
        const double currentAngle = std::atan2(currentVector.y(), currentVector.x());
        double degrees = (currentAngle - startAngle) * 180.0
            / 3.14159265358979323846;
        if (modifiers.testFlag(Qt::ShiftModifier)) {
            degrees = std::round(degrees / 15.0) * 15.0;
        }
        QTransform rotation;
        rotation.rotate(degrees);
        const QTransform aroundPivot = QTransform::fromTranslate(-center.x(), -center.y())
            * rotation
            * QTransform::fromTranslate(center.x(), center.y());
        return base * aroundPivot;
    }

    const bool skewMode = m_transformMode == CanvasTransformMode::SkewTop
        || m_transformMode == CanvasTransformMode::SkewRight
        || m_transformMode == CanvasTransformMode::SkewBottom
        || m_transformMode == CanvasTransformMode::SkewLeft;
    const int controlPoint = transformControlPointIndex(m_transformMode);
    if (skewMode || controlPoint >= 0) {
        QPolygonF target = m_transformGestureBaseQuad;
        const QPolygonF source = transformSourceQuad();
        if (source.size() != 4 || target.size() != 4) {
            clearTransformSnapState();
            return base;
        }

        if (skewMode) {
            int first = 0;
            int second = 1;
            switch (m_transformMode) {
            case CanvasTransformMode::SkewTop: first = 0; second = 1; break;
            case CanvasTransformMode::SkewRight: first = 1; second = 2; break;
            case CanvasTransformMode::SkewBottom: first = 3; second = 2; break;
            case CanvasTransformMode::SkewLeft: first = 0; second = 3; break;
            default: break;
            }
            const QPointF tangent = target.at(second) - target.at(first);
            const double tangentLengthSquared = QPointF::dotProduct(tangent, tangent);
            if (tangentLengthSquared <= 1.0e-9) {
                clearTransformSnapState();
                return base;
            }
            const QPointF rawDelta = documentPosition - m_transformStartDocumentPosition;
            QPointF projected = tangent
                * (QPointF::dotProduct(rawDelta, tangent) / tangentLengthSquared);
            QPointF midpoint = (target.at(first) + target.at(second)) * 0.5 + projected;
            if (snapping) {
                const TransformAxisSnap xSnap = resolveTransformAxisSnap(
                    Qt::Vertical, {midpoint.x()});
                const TransformAxisSnap ySnap = resolveTransformAxisSnap(
                    Qt::Horizontal, {midpoint.y()});
                const QPointF corrected = projected
                    + QPointF(xSnap.correction, ySnap.correction);
                projected = tangent
                    * (QPointF::dotProduct(corrected, tangent)
                       / tangentLengthSquared);
            } else {
                clearTransformSnapState();
            }
            target[first] += projected;
            target[second] += projected;
        } else {
            QPointF pointer = documentPosition;
            if (snapping) {
                const TransformAxisSnap xSnap = resolveTransformAxisSnap(
                    Qt::Vertical, {pointer.x()});
                const TransformAxisSnap ySnap = resolveTransformAxisSnap(
                    Qt::Horizontal, {pointer.y()});
                pointer += QPointF(xSnap.correction, ySnap.correction);
            } else {
                clearTransformSnapState();
            }
            QPointF delta = pointer - m_transformStartDocumentPosition;
            if (modifiers.testFlag(Qt::ShiftModifier)
                && m_transformInteractionMode
                       == CanvasTransformInteractionMode::Perspective) {
                if (std::abs(delta.x()) >= std::abs(delta.y())) {
                    delta.setY(0.0);
                } else {
                    delta.setX(0.0);
                }
            }

            target[controlPoint] += delta;
            if (m_transformInteractionMode
                == CanvasTransformInteractionMode::Perspective) {
                // Coupled perspective keeps the diagonal corner fixed while
                // moving the two adjacent corners symmetrically. Horizontal
                // motion changes convergence of the selected row; vertical
                // motion changes convergence of the selected column.
                const int previous = (controlPoint + 3) % 4;
                const int next = (controlPoint + 1) % 4;
                if (controlPoint == 0 || controlPoint == 2) {
                    target[next] += QPointF(-delta.x(), delta.y());
                    target[previous] += QPointF(delta.x(), -delta.y());
                } else {
                    target[previous] += QPointF(-delta.x(), delta.y());
                    target[next] += QPointF(delta.x(), -delta.y());
                }
            }
        }

        QTransform result;
        if (!transformFromQuad(source, target, &result)) {
            return base;
        }
        bool baseInvertible = false;
        const QTransform baseInverse = base.inverted(&baseInvertible);
        if (baseInvertible) {
            const QPointF pivotSource = baseInverse.map(
                m_transformGestureStartPivotDocument);
            m_transformPivotDocument = result.map(pivotSource);
        }
        return result;
    }

    bool invertible = false;
    const QTransform baseInverse = base.inverted(&invertible);
    if (!invertible) {
        clearTransformSnapState();
        return base;
    }

    const QPointF anchor = m_transformAnchorDocumentPosition;
    const QPointF start = m_transformStartHandleDocumentPosition;
    const bool scalesX = m_transformMode == CanvasTransformMode::ScaleTopLeft
        || m_transformMode == CanvasTransformMode::ScaleTopRight
        || m_transformMode == CanvasTransformMode::ScaleRight
        || m_transformMode == CanvasTransformMode::ScaleBottomRight
        || m_transformMode == CanvasTransformMode::ScaleBottomLeft
        || m_transformMode == CanvasTransformMode::ScaleLeft;
    const bool scalesY = m_transformMode == CanvasTransformMode::ScaleTopLeft
        || m_transformMode == CanvasTransformMode::ScaleTop
        || m_transformMode == CanvasTransformMode::ScaleTopRight
        || m_transformMode == CanvasTransformMode::ScaleBottomRight
        || m_transformMode == CanvasTransformMode::ScaleBottom
        || m_transformMode == CanvasTransformMode::ScaleBottomLeft;

    QPointF pointer = documentPosition;
    const bool axisAligned = std::abs(base.m12()) < 1.0e-9
        && std::abs(base.m21()) < 1.0e-9
        && std::abs(base.m13()) < 1.0e-12
        && std::abs(base.m23()) < 1.0e-12;
    if (snapping && axisAligned) {
        // Axis-aligned resize handles terminate on pixel boundaries. The
        // unscaled axis remains untouched for edge handles. Rotated, skewed
        // and projective geometry keeps its existing subpixel pointer path.
        if (scalesX) pointer.setX(std::round(pointer.x()));
        if (scalesY) pointer.setY(std::round(pointer.y()));
    }
    TransformAxisSnap xSnap;
    TransformAxisSnap ySnap;
    if (snapping && axisAligned && scalesX) {
        xSnap = resolveTransformAxisSnap(
            Qt::Vertical, {pointer.x()}, false, true);
        pointer.rx() += xSnap.correction;
    } else {
        resolveTransformAxisSnap(Qt::Vertical, {});
    }
    if (snapping && axisAligned && scalesY) {
        ySnap = resolveTransformAxisSnap(
            Qt::Horizontal, {pointer.y()}, false, true);
        pointer.ry() += ySnap.correction;
    } else {
        resolveTransformAxisSnap(Qt::Horizontal, {});
    }

    QPointF localPointer = baseInverse.map(pointer);
    double scaleX = 1.0;
    double scaleY = 1.0;
    const auto resolveScale = [](const double pointerValue,
                                 const double anchorValue,
                                 const double startValue) {
        return std::abs(startValue - anchorValue) > 1.0e-6
            ? (pointerValue - anchorValue) / (startValue - anchorValue)
            : 1.0;
    };
    if (scalesX) {
        scaleX = resolveScale(localPointer.x(), anchor.x(), start.x());
        if (scaleX < 0.01 && xSnap.active) {
            resolveTransformAxisSnap(Qt::Vertical, {});
            xSnap = {};
            pointer.setX(snapping && axisAligned
                             ? std::round(documentPosition.x())
                             : documentPosition.x());
            localPointer = baseInverse.map(pointer);
            scaleX = resolveScale(localPointer.x(), anchor.x(), start.x());
        }
    }
    if (scalesY) {
        scaleY = resolveScale(localPointer.y(), anchor.y(), start.y());
        if (scaleY < 0.01 && ySnap.active) {
            resolveTransformAxisSnap(Qt::Horizontal, {});
            ySnap = {};
            pointer.setY(snapping && axisAligned
                             ? std::round(documentPosition.y())
                             : documentPosition.y());
            localPointer = baseInverse.map(pointer);
            scaleY = resolveScale(localPointer.y(), anchor.y(), start.y());
        }
    }
    scaleX = scalesX ? std::max(0.01, scaleX) : 1.0;
    scaleY = scalesY ? std::max(0.01, scaleY) : 1.0;

    if (modifiers.testFlag(Qt::ShiftModifier) && scalesX && scalesY) {
        bool useX = xSnap.active;
        bool useY = ySnap.active;
        if (useX && useY && std::abs(scaleX - scaleY) > 1.0e-6) {
            if (xSnap.screenDistance <= ySnap.screenDistance) {
                useY = false;
                resolveTransformAxisSnap(Qt::Horizontal, {});
            } else {
                useX = false;
                resolveTransformAxisSnap(Qt::Vertical, {});
            }
        }
        const double uniform = useX
            ? scaleX
            : useY
                ? scaleY
                : std::abs(scaleX - 1.0) >= std::abs(scaleY - 1.0)
                    ? scaleX : scaleY;
        scaleX = uniform;
        scaleY = uniform;
    }

    QTransform scale;
    scale.scale(scaleX, scaleY);
    const QTransform localScale = QTransform::fromTranslate(-anchor.x(), -anchor.y())
        * scale
        * QTransform::fromTranslate(anchor.x(), anchor.y());
    const QTransform result = localScale * base;
    const QPointF pivotLocal = baseInverse.map(m_transformGestureStartPivotDocument);
    m_transformPivotDocument = result.map(pivotLocal);
    return result;
}

void ImageCanvas::updateTransformCursor(const QPointF &viewportPosition)
{
    switch (transformModeAt(viewportPosition)) {
    case CanvasTransformMode::Move:
        viewport()->setCursor(Qt::SizeAllCursor);
        break;
    case CanvasTransformMode::ScaleTop:
    case CanvasTransformMode::ScaleBottom:
        viewport()->setCursor(Qt::SizeVerCursor);
        break;
    case CanvasTransformMode::ScaleLeft:
    case CanvasTransformMode::ScaleRight:
    case CanvasTransformMode::SkewTop:
    case CanvasTransformMode::SkewBottom:
        viewport()->setCursor(Qt::SizeHorCursor);
        break;
    case CanvasTransformMode::ScaleTopLeft:
    case CanvasTransformMode::ScaleBottomRight:
        viewport()->setCursor(Qt::SizeFDiagCursor);
        break;
    case CanvasTransformMode::ScaleTopRight:
    case CanvasTransformMode::ScaleBottomLeft:
        viewport()->setCursor(Qt::SizeBDiagCursor);
        break;
    case CanvasTransformMode::SkewLeft:
    case CanvasTransformMode::SkewRight:
        viewport()->setCursor(Qt::SizeVerCursor);
        break;
    case CanvasTransformMode::Rotate:
    case CanvasTransformMode::Pivot:
    case CanvasTransformMode::ControlTopLeft:
    case CanvasTransformMode::ControlTopRight:
    case CanvasTransformMode::ControlBottomRight:
    case CanvasTransformMode::ControlBottomLeft:
        viewport()->setCursor(Qt::CrossCursor);
        break;
    case CanvasTransformMode::None:
        viewport()->setCursor(m_toolCursor);
        break;
    }
}

void ImageCanvas::updateScrollBars()
{
    if (!hasImage() || m_fitMode) {
        horizontalScrollBar()->setRange(0, 0);
        verticalScrollBar()->setRange(0, 0);
        return;
    }

    const int scaledWidth = std::max(0, qRound(m_image.width() * m_zoom));
    const int scaledHeight = std::max(0, qRound(m_image.height() * m_zoom));
    horizontalScrollBar()->setPageStep(viewport()->width());
    verticalScrollBar()->setPageStep(viewport()->height());

    // A range equal to the scaled image length, combined with the half-view
    // origin used by imageRect(), provides exactly half a viewport of useful
    // overscroll at both ends.
    horizontalScrollBar()->setRange(0, scaledWidth);
    verticalScrollBar()->setRange(0, scaledHeight);
}

void ImageCanvas::updateRulerGeometry()
{
    if (!m_rulersVisible) {
        return;
    }

    const QRect viewportGeometry = viewport()->geometry();
    m_horizontalRuler->setGeometry(viewportGeometry.x(),
                                   viewportGeometry.y() - m_rulerThickness,
                                   viewportGeometry.width(),
                                   m_rulerThickness);
    m_verticalRuler->setGeometry(viewportGeometry.x() - m_rulerThickness,
                                 viewportGeometry.y(),
                                 m_rulerThickness,
                                 viewportGeometry.height());
    m_rulerCorner->setGeometry(viewportGeometry.x() - m_rulerThickness,
                               viewportGeometry.y() - m_rulerThickness,
                               m_rulerThickness,
                               m_rulerThickness);
    m_horizontalRuler->raise();
    m_verticalRuler->raise();
    m_rulerCorner->raise();
    updateRulers();
}

void ImageCanvas::updateRulers()
{
    if (m_horizontalRuler) {
        m_horizontalRuler->update();
    }
    if (m_verticalRuler) {
        m_verticalRuler->update();
    }
}

bool ImageCanvas::panGestureActive(const QMouseEvent *event) const
{
    return event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton && (m_spaceHeld || m_leftDragPans));
}

double ImageCanvas::documentScale(const Qt::Orientation orientation) const
{
    if (!hasImage() || !m_documentSize.isValid() || m_documentSize.isEmpty()) {
        return 1.0;
    }
    const QRectF target = imageRect();
    if (orientation == Qt::Horizontal) {
        return target.height() / std::max(1, m_documentSize.height());
    }
    return target.width() / std::max(1, m_documentSize.width());
}

double ImageCanvas::documentCoordinate(const Qt::Orientation guideOrientation,
                                       const double viewportPosition) const
{
    const QRectF target = imageRect();
    const double scale = documentScale(guideOrientation);
    if (guideOrientation == Qt::Vertical) {
        return (viewportPosition - target.left()) / scale;
    }
    return (viewportPosition - target.top()) / scale;
}

double ImageCanvas::viewportCoordinate(const Qt::Orientation guideOrientation,
                                       const double documentPosition) const
{
    const QRectF target = imageRect();
    const double scale = documentScale(guideOrientation);
    if (guideOrientation == Qt::Vertical) {
        return target.left() + documentPosition * scale;
    }
    return target.top() + documentPosition * scale;
}

double ImageCanvas::snapGuidePosition(const Qt::Orientation orientation,
                                      const double position,
                                      bool *snapped) const
{
    if (snapped) {
        *snapped = false;
    }

    const double maximum = orientation == Qt::Vertical
        ? m_documentSize.width()
        : m_documentSize.height();
    double result = std::clamp(position, 0.0, maximum);
    if (!m_snapGuides) {
        return result;
    }

    // Guides use a half-pixel document lattice: integer coordinates are pixel
    // edges and x.5/y.5 coordinates are pixel centres. This also gives odd-size
    // documents an exact centre guide (for example 63.5 in a 127 px image).
    const double unsnappedResult = result;
    result = snapGuideCoordinate(result, maximum);
    if (snapped && std::abs(result - unsnappedResult) > 1.0e-9) {
        *snapped = true;
    }

    QVector<double> candidates;
    candidates.reserve(3 + m_snapBounds.size() * 3);
    candidates << 0.0 << maximum * 0.5 << maximum;
    for (const QRectF &bounds : m_snapBounds) {
        if (orientation == Qt::Vertical) {
            candidates << bounds.left() << bounds.center().x() << bounds.right();
        } else {
            candidates << bounds.top() << bounds.center().y() << bounds.bottom();
        }
    }

    const double scale = documentScale(orientation);
    double bestScreenDistance = GuideSnapTolerance + 1.0;
    for (const double candidate : candidates) {
        const double clampedCandidate = snapGuideCoordinate(candidate, maximum);
        const double screenDistance = std::abs(clampedCandidate - result) * scale;
        if (screenDistance <= GuideSnapTolerance && screenDistance < bestScreenDistance) {
            result = clampedCandidate;
            bestScreenDistance = screenDistance;
            if (snapped) {
                *snapped = true;
            }
        }
    }
    return result;
}

ImageCanvas::GuideHit ImageCanvas::guideAt(const QPointF &position,
                                           const double tolerance) const
{
    GuideHit best;
    double bestDistance = tolerance + 1.0;

    for (int index = 0; index < m_verticalGuides.size(); ++index) {
        const double distance = std::abs(position.x()
                                         - viewportCoordinate(Qt::Vertical,
                                                              m_verticalGuides.at(index)));
        if (distance <= tolerance && distance < bestDistance) {
            best.orientation = Qt::Vertical;
            best.index = index;
            bestDistance = distance;
        }
    }
    for (int index = 0; index < m_horizontalGuides.size(); ++index) {
        const double distance = std::abs(position.y()
                                         - viewportCoordinate(Qt::Horizontal,
                                                              m_horizontalGuides.at(index)));
        if (distance <= tolerance && distance < bestDistance) {
            best.orientation = Qt::Horizontal;
            best.index = index;
            bestDistance = distance;
        }
    }
    return best;
}

void ImageCanvas::removeGuide(const GuideHit &hit)
{
    if (!hit.isValid()) {
        return;
    }
    if (hit.orientation == Qt::Vertical && hit.index < m_verticalGuides.size()) {
        m_verticalGuides.removeAt(hit.index);
    } else if (hit.orientation == Qt::Horizontal && hit.index < m_horizontalGuides.size()) {
        m_horizontalGuides.removeAt(hit.index);
    } else {
        return;
    }
    viewport()->update();
    commitGuidesChanged();
}

void ImageCanvas::beginGuideFromRuler(const Qt::Orientation guideOrientation,
                                     const double viewportPosition)
{
    if (!hasImage()) {
        return;
    }
    setGuidesVisible(true);
    m_draggingGuide = true;
    m_draggingNewGuide = true;
    m_dragGuideOrientation = guideOrientation;
    m_dragGuideIndex = -1;
    m_dragGuidePosition = snapGuidePosition(
        guideOrientation,
        documentCoordinate(guideOrientation, viewportPosition),
        &m_dragGuideSnapped);
    viewport()->update();
}

void ImageCanvas::updateGuideFromRuler(const double viewportPosition)
{
    if (!m_draggingGuide || !m_draggingNewGuide) {
        return;
    }
    m_dragGuidePosition = snapGuidePosition(
        m_dragGuideOrientation,
        documentCoordinate(m_dragGuideOrientation, viewportPosition),
        &m_dragGuideSnapped);
    viewport()->update();
}

void ImageCanvas::finishGuideFromRuler(const bool commit)
{
    if (!m_draggingGuide || !m_draggingNewGuide) {
        return;
    }

    if (commit) {
        QVector<double> &guides = m_dragGuideOrientation == Qt::Vertical
            ? m_verticalGuides
            : m_horizontalGuides;
        guides.push_back(m_dragGuidePosition);
        sortAndDeduplicate(guides);
        commitGuidesChanged();
    }
    cancelGuideDrag();
}

void ImageCanvas::beginExistingGuideDrag(const GuideHit &hit)
{
    if (!hit.isValid()) {
        return;
    }
    m_draggingGuide = true;
    m_draggingNewGuide = false;
    m_dragGuideOrientation = hit.orientation;
    m_dragGuideIndex = hit.index;
    m_dragGuidePosition = hit.orientation == Qt::Vertical
        ? m_verticalGuides.at(hit.index)
        : m_horizontalGuides.at(hit.index);
    m_dragGuideSnapped = false;
    viewport()->setCursor(hit.orientation == Qt::Vertical
                              ? Qt::SizeHorCursor
                              : Qt::SizeVerCursor);
    viewport()->update();
}

void ImageCanvas::updateExistingGuideDrag(const QPointF &position)
{
    if (!m_draggingGuide || m_draggingNewGuide) {
        return;
    }
    const double viewportPosition = m_dragGuideOrientation == Qt::Vertical
        ? position.x()
        : position.y();
    m_dragGuidePosition = snapGuidePosition(
        m_dragGuideOrientation,
        documentCoordinate(m_dragGuideOrientation, viewportPosition),
        &m_dragGuideSnapped);
    viewport()->update();
}

void ImageCanvas::finishExistingGuideDrag(const QPointF &position)
{
    if (!m_draggingGuide || m_draggingNewGuide) {
        return;
    }

    const bool returnedToRuler = m_dragGuideOrientation == Qt::Vertical
        ? position.x() < 0.0
        : position.y() < 0.0;
    QVector<double> &guides = m_dragGuideOrientation == Qt::Vertical
        ? m_verticalGuides
        : m_horizontalGuides;
    if (m_dragGuideIndex >= 0 && m_dragGuideIndex < guides.size()) {
        if (returnedToRuler) {
            guides.removeAt(m_dragGuideIndex);
        } else {
            guides[m_dragGuideIndex] = m_dragGuidePosition;
            sortAndDeduplicate(guides);
        }
        commitGuidesChanged();
    }
    cancelGuideDrag();
}

void ImageCanvas::cancelGuideDrag()
{
    m_draggingGuide = false;
    m_draggingNewGuide = false;
    m_dragGuideSnapped = false;
    m_dragGuideIndex = -1;
    viewport()->setCursor(m_spaceHeld ? Qt::OpenHandCursor : m_toolCursor);
    viewport()->update();
}

void ImageCanvas::commitGuidesChanged()
{
    emit guidesChanged(m_horizontalGuides, m_verticalGuides);
}

void ImageCanvas::paintRuler(QPainter &painter,
                            const Qt::Orientation rulerOrientation,
                            const QRect &rect) const
{
    painter.fillRect(rect, themeColour(QStringLiteral("panel_alt")));
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(themeColour(QStringLiteral("border")));
    if (rulerOrientation == Qt::Horizontal) {
        painter.drawLine(rect.bottomLeft(), rect.bottomRight());
    } else {
        painter.drawLine(rect.topRight(), rect.bottomRight());
    }

    if (!hasImage() || !m_documentSize.isValid()) {
        return;
    }

    const Qt::Orientation guideOrientation = rulerOrientation == Qt::Horizontal
        ? Qt::Vertical
        : Qt::Horizontal;
    const double scale = documentScale(guideOrientation);
    if (scale <= 0.0) {
        return;
    }

    const QRectF target = imageRect();
    const double origin = rulerOrientation == Qt::Horizontal ? target.left() : target.top();
    const double screenLength = rulerOrientation == Qt::Horizontal ? rect.width() : rect.height();
    const double minimum = (0.0 - origin) / scale;
    const double maximum = (screenLength - origin) / scale;
    const double majorStep = niceMajorStep(scale);
    const double minorStep = majorStep / 5.0;
    const int majorEvery = 5;
    const qint64 firstIndex = static_cast<qint64>(std::floor(minimum / minorStep)) - 1;
    const qint64 lastIndex = static_cast<qint64>(std::ceil(maximum / minorStep)) + 1;

    painter.setFont(QFont(painter.font().family(), 7));
    const QColor tickColour = themeColour(QStringLiteral("text_muted"));
    const QColor labelColour = themeColour(QStringLiteral("text"));

    for (qint64 index = firstIndex; index <= lastIndex; ++index) {
        const double value = index * minorStep;
        const double screen = origin + value * scale;
        const bool major = index % majorEvery == 0;
        const int tickLength = major ? 12 : 6;
        painter.setPen(major ? labelColour : tickColour);

        if (rulerOrientation == Qt::Horizontal) {
            painter.drawLine(QPointF(screen, rect.bottom()),
                             QPointF(screen, rect.bottom() - tickLength));
            if (major) {
                painter.drawText(QPointF(screen + 3.0, 9.0), rulerLabel(value));
            }
        } else {
            painter.drawLine(QPointF(rect.right(), screen),
                             QPointF(rect.right() - tickLength, screen));
            if (major) {
                painter.save();
                painter.translate(9.0, screen - 3.0);
                painter.rotate(-90.0);
                painter.drawText(QPointF(0.0, 0.0), rulerLabel(value));
                painter.restore();
            }
        }
    }
}

} // namespace vfx
