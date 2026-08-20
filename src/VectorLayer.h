#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QTransform>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

namespace vfx {

enum class VectorShapeType {
    Rectangle,
    RoundedRectangle,
    Ellipse,
    Line,
    Polygon,
    Star,
    Arrow,
    Path
};

enum class VectorStrokeAlignment {
    Inside,
    Centre,
    Outside
};

enum class VectorStrokeCap {
    Butt,
    Round,
    Square
};

enum class VectorStrokeJoin {
    Miter,
    Round,
    Bevel
};

enum class VectorStrokePattern {
    Solid,
    Dashed
};

enum class VectorArrowheadType {
    None,
    Open,
    Triangle,
    Stealth,
    Diamond,
    Circle
};

enum class VectorPathFillRule {
    EvenOdd,
    NonZero
};

QString vectorShapeTypeToString(VectorShapeType type);
QString vectorShapeTypeDisplayName(VectorShapeType type);
VectorShapeType vectorShapeTypeFromString(const QString &value, bool *ok = nullptr);
QString vectorStrokeAlignmentDisplayName(VectorStrokeAlignment alignment);
QString vectorStrokeCapDisplayName(VectorStrokeCap cap);
QString vectorStrokeJoinDisplayName(VectorStrokeJoin join);
QString vectorStrokePatternDisplayName(VectorStrokePattern pattern);
QString vectorArrowheadTypeToString(VectorArrowheadType type);
QString vectorArrowheadTypeDisplayName(VectorArrowheadType type);
VectorArrowheadType vectorArrowheadTypeFromString(const QString &value,
                                                  bool *ok = nullptr);

enum class VectorNodeMode {
    Corner,
    Smooth,
    Symmetric
};

enum class VectorCornerStyle {
    Rounded,
    Chamfer,
    Concave,
    Cutout
};

QString vectorNodeModeToString(VectorNodeMode mode);
QString vectorNodeModeDisplayName(VectorNodeMode mode);
VectorNodeMode vectorNodeModeFromString(const QString &value, bool *ok = nullptr);
QString vectorCornerStyleToString(VectorCornerStyle style);
QString vectorCornerStyleDisplayName(VectorCornerStyle style);
VectorCornerStyle vectorCornerStyleFromString(const QString &value, bool *ok = nullptr);

struct VectorPathNode {
    QUuid id = QUuid::createUuid();
    QPointF anchor;
    QPointF inHandle;
    QPointF outHandle;
    bool inHandleActive = false;
    bool outHandleActive = false;
    VectorNodeMode mode = VectorNodeMode::Corner;
    double cornerRadius = 0.0;
    VectorCornerStyle cornerStyle = VectorCornerStyle::Rounded;

    void normalise();
    bool isSafe() const;
    void moveBy(const QPointF &delta);
    void setInHandle(const QPointF &position, bool preserveOpposite = true);
    void setOutHandle(const QPointF &position, bool preserveOpposite = true);
    void clearHandles();
    void makeSharp();
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorPathNode fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const VectorPathNode &) const = default;
};

struct VectorBezierPath {
    static constexpr int MaximumNodeCount = 100000;

    bool closed = false;
    QVector<VectorPathNode> nodes;

    void normalise();
    bool isSafe() const;
    int segmentCount() const;
    bool transformNodes(const QSet<int> &nodeIndices,
                        const QTransform &pointTransform);
    bool moveNodesBy(const QSet<int> &nodeIndices, const QPointF &delta);
    void reverseDirection();
    bool joinFollowingPath(const VectorBezierPath &following,
                           int *junctionNodeIndex = nullptr,
                           double coincidentTolerance = 1.0e-7);
    QPainterPath painterPath() const;
    QPainterPath basePainterPath() const;
    QRectF contentBounds() const;
    QVector<QPointF> snapPoints() const;
    bool cornerableNode(int nodeIndex) const;
    double maximumCornerRadius(int nodeIndex) const;
    QPointF cornerHandlePoint(int nodeIndex) const;
    bool hasCornerMetadata() const;
    bool hasLiveCorners() const;
    bool bakeCorners();
    static VectorBezierPath fromPainterPath(const QPainterPath &path,
                                            bool closed,
                                            bool *ok = nullptr);
    bool insertNodeOnSegment(int segmentIndex, double t, int *insertedIndex = nullptr);
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorBezierPath fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const VectorBezierPath &) const = default;
};

struct VectorFill {
    bool enabled = true;
    QColor colour = QColor(Qt::black);
    double opacity = 1.0;

    void normalise();
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorFill fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const VectorFill &) const = default;
};

struct VectorStroke {
    bool enabled = false;
    QColor colour = QColor(Qt::black);
    double opacity = 1.0;
    double width = 1.0;
    VectorStrokeAlignment alignment = VectorStrokeAlignment::Centre;
    VectorStrokeCap cap = VectorStrokeCap::Butt;
    VectorStrokeJoin join = VectorStrokeJoin::Miter;
    double miterLimit = 4.0;
    VectorStrokePattern pattern = VectorStrokePattern::Solid;
    double dashLength = 8.0;
    double gapLength = 8.0;
    double dashOffset = 0.0;
    VectorArrowheadType startArrowhead = VectorArrowheadType::None;
    VectorArrowheadType endArrowhead = VectorArrowheadType::None;
    double startArrowScale = 1.0;
    double endArrowScale = 1.0;

    void normalise(bool openPath = false);
    bool isSafe(bool openPath = false) const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorStroke fromJson(const QJsonObject &object,
                                 bool openPath,
                                 bool *ok = nullptr);
    bool operator==(const VectorStroke &) const = default;
};

struct VectorCornerRadii {
    double topLeft = 16.0;
    double topRight = 16.0;
    double bottomRight = 16.0;
    double bottomLeft = 16.0;

    void setAll(double radius);
    void sanitise();
    void normalise(const QSizeF &boundsSize);
    QSizeF minimumSize() const;
    bool isSafe() const;
    bool allEqual(double epsilon = 1.0e-9) const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorCornerRadii fromJson(const QJsonObject &object,
                                      bool *ok = nullptr);
    bool operator==(const VectorCornerRadii &) const = default;
};

struct VectorShape {
    QUuid id = QUuid::createUuid();
    VectorShapeType type = VectorShapeType::Rectangle;
    QRectF bounds {0.0, 0.0, 100.0, 100.0};
    // Line geometry remains explicit so reverse-slope, horizontal and vertical
    // lines survive creation, transforms and save/reopen without being inferred
    // from a normalised rectangle.
    QPointF lineStart {0.0, 0.0};
    QPointF lineEnd {100.0, 100.0};
    int polygonSides = 5;
    double starInnerRatio = 0.5;
    double vertexRotationDegrees = -90.0;
    // Semantic block-arrow proportions, measured relative to the local bounds.
    // The arrow points towards the right edge before object/layer transforms.
    double arrowHeadLengthRatio = 0.35;
    double arrowShaftWidthRatio = 0.35;
    VectorBezierPath bezierPath;
    // Closed Path objects may contain additional independent contours. This
    // is required for true compound fills such as stroke rings (outer contour
    // plus an inner hole) and expanded dashed strokes. The primary contour
    // remains in bezierPath for backward compatibility and existing editing
    // workflows; additional contours never use synthetic connector segments.
    QVector<VectorBezierPath> additionalBezierPaths;
    // Compound paths retain the authored fill rule. EvenOdd is the historic
    // default used by existing Photo Lab paths; NonZero is required for
    // expanded dashed strokes whose neighbouring contours may overlap.
    VectorPathFillRule pathFillRule = VectorPathFillRule::EvenOdd;
    VectorCornerRadii cornerRadii;
    bool cornerRadiiLinked = true;
    VectorFill fill;
    VectorStroke stroke;
    QTransform transform;
    quint64 revision = 1;

    bool isOpenPath() const;
    void normalise();
    bool isSafe() const;
    QPainterPath geometryPath() const;
    QPainterPath path() const;
    QPainterPath pathForWorldTransform(const QTransform &worldTransform) const;
    QPainterPath strokeOutlineForWorldTransform(const QTransform &worldTransform,
                                                double widthMultiplier = 1.0) const;
    QPainterPath strokePathForWorldTransform(const QTransform &worldTransform) const;
    QPainterPath styledPathForWorldTransform(const QTransform &worldTransform) const;
    QSizeF orthogonalWorldSize(const QTransform &worldTransform,
                               bool *ok = nullptr) const;
    bool cornerRadiiFitWorldTransform(const QTransform &worldTransform,
                                      double epsilon = 1.0e-6) const;
    // Replace the semantic primitive geometry with ordinary editable Bézier
    // nodes while retaining this object's identity, appearance and transform.
    // The supplied world transform is the layer/parent transform outside the
    // object transform. Rounded rectangles use it to bake the currently
    // visible document-pixel corner radii instead of stretching their local
    // radii under a non-uniform transform.
    bool convertToPath(const QTransform &worldTransform = QTransform());
    // Resolve the currently visible stroke in document space, including
    // alignment, caps, joins, miter limits and dashes, then bake it back into
    // one ordinary closed compound fill path in the containing layer's
    // coordinate system. Disconnected dashes and closed stroke holes remain
    // independent contours. Expanded outlines use NonZero fill so overlapping
    // dash islands remain a union rather than cancelling under odd-even fill.
    // The output carries the stroke colour/opacity as its fill and has no
    // stroke or object transform.
    bool expandedStrokePath(const QTransform &worldTransform,
                            VectorShape *expanded) const;
    QRectF contentBounds() const;
    QVector<QPointF> snapPoints(const QTransform &worldTransform = QTransform()) const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorShape fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const VectorShape &) const = default;
};

struct VectorAppearance {
    static constexpr quint32 CurrentSchema = 2;

    quint32 schema = CurrentSchema;
    VectorFill fill;
    VectorStroke stroke;

    void normalise();
    bool isSafe() const;
    void applyTo(VectorShape &shape) const;
    void swapFillAndStroke();
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorAppearance fromJson(const QJsonObject &object,
                                     bool *ok = nullptr);
    static VectorAppearance fromShape(const VectorShape &shape);
    static VectorAppearance sensibleDefaults(const QColor &primaryColour,
                                              const QColor &secondaryColour,
                                              bool openPath);
    bool operator==(const VectorAppearance &) const = default;
};

struct VectorLayerData {
    static constexpr quint32 CurrentSchema = 8;
    static constexpr int MaximumObjectCount = 10000;
    // Feather is stored in document pixels and remains a layer-level vector
    // appearance property. The deliberately generous persisted safety bound
    // keeps malformed payloads finite without silently changing valid values.
    static constexpr double MaximumFeatherRadius = 1'000'000.0;
    // Bound cumulative editable path complexity per layer, not only each
    // individual shape, so malformed payloads cannot multiply the per-path
    // node limit across thousands of otherwise valid objects.
    static constexpr qsizetype MaximumTotalNodeCount = 1000000;

    quint32 schema = CurrentSchema;
    double featherRadius = 0.0;
    QVector<VectorShape> objects;

    void normalise();
    bool isSafe() const;
    QRectF contentBounds() const;
    QVector<QPointF> snapPoints(const QTransform &worldTransform = QTransform()) const;
    quint64 fingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static VectorLayerData fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const VectorLayerData &) const = default;
};

} // namespace vfx
