#include "SvgWorkflow.h"

#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace vfx;

class SvgTests final : public QObject {
    Q_OBJECT

private slots:
    void importsCommonSvgGeometry();
    void exactRoundTripRetainsSemanticLayers();
    void preservesSvgTransformAndViewBoxOrder();
    void usesStandaloneSvgDefaultViewport();
    void skipsSingularLocalTransforms();
    void preservesVisibilityOverridesInsideHiddenGroups();
    void importsSupportedElementBlendModes();
    void skipsSymbolDefinitionsWithoutUseSupport();
    void handlesInvisibleAndStrokeOnlyTextHonestly();
    void damagedMetadataFallsBackToStandardGeometry();
    void selectedNestedExportKeepsWorldTransform();
    void closedPathExportDeclaresFillRule();
    void liveCornerExportUsesVisibleGeometryAndExactMetadata();
    void dashedStrokeImportsExportsAndPreservesCaps();
    void arrowheadsAndArrowShapeRoundTripWithSvgMarkers();
    void compoundPathImportsExportsWithoutConnectorGeometry();
    void rejectsMalformedArcFlagsWithoutLosingSiblings();
    void boundsCumulativeImportAndExportComplexity();
    void rejectsUnsafeExportHierarchy();
    void rejectsEntityDeclarations();
};

void SvgTests::importsCommonSvgGeometry()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="180" viewBox="0 0 160 90">
      <g transform="translate(5 3)" opacity="0.8">
        <rect id="panel" x="10" y="10" width="60" height="30" rx="6" fill="#336699" stroke="white" stroke-width="2"/>
        <path id="curve" d="M 5 70 C 40 15 95 120 150 30 A 12 8 25 0 1 155 50" fill="none" stroke="red"/>
        <text id="label" x="20" y="25" font-size="14">SVG test</text>
      </g>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Sample"), &result, &error), qPrintable(error));
    QCOMPARE(result.canvasSize, QSize(320, 180));
    QCOMPARE(result.layers.size(), 1);
    QCOMPARE(result.layers.constFirst().type, LayerType::Group);
    QVERIFY(result.importedLayerCount >= 5);
    QVERIFY(result.layers.constFirst().transform.isInvertible());
}

void SvgTests::exactRoundTripRetainsSemanticLayers()
{
    LayerNode vector;
    vector.type = LayerType::Vector;
    vector.name = QStringLiteral("Rounded card");
    vector.transform.translate(24.0, 12.0);
    VectorShape shape;
    shape.type = VectorShapeType::RoundedRectangle;
    shape.bounds = QRectF(10.0, 20.0, 180.0, 90.0);
    shape.cornerRadii = {8.0, 14.0, 22.0, 4.0};
    shape.cornerRadiiLinked = false;
    shape.fill.colour = QColor(10, 80, 180);
    shape.stroke.enabled = true;
    shape.stroke.alignment = VectorStrokeAlignment::Inside;
    shape.stroke.width = 5.0;
    shape.normalise();
    vector.vectorData.objects = {shape};
    vector.vectorData.normalise();

    LayerNode text;
    text.type = LayerType::Text;
    text.name = QStringLiteral("Caption");
    text.textData.text = QStringLiteral("Hello\nSVG");
    text.textData.origin = QPointF(44.0, 120.0);
    text.textData.fontSize = 22.0;
    text.textData.normalise();

    SvgExportResult exportResult;
    QString error;
    const QByteArray svg = SvgWorkflow::exportData(QSize(400, 240), {vector, text}, {},
                                                   &exportResult, &error);
    QVERIFY2(!svg.isEmpty(), qPrintable(error));
    QCOMPARE(exportResult.exportedLayerCount, 2);

    SvgImportResult imported;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("RoundTrip"), &imported, &error), qPrintable(error));
    QCOMPARE(imported.canvasSize, QSize(400, 240));
    QCOMPARE(imported.layers.size(), 1);
    const LayerNode root = imported.layers.constFirst();
    QCOMPARE(root.children.size(), 2);
    const LayerNode importedVector = root.children.at(0);
    const LayerNode importedText = root.children.at(1);
    QCOMPARE(importedVector.type, LayerType::Vector);
    QCOMPARE(importedText.type, LayerType::Text);
    QCOMPARE(importedVector.vectorData.objects.size(), 1);
    QVERIFY(importedVector.vectorData.objects.constFirst().cornerRadii
            == shape.cornerRadii);
    QCOMPARE(importedVector.vectorData.objects.constFirst().stroke.alignment,
             VectorStrokeAlignment::Inside);
    QCOMPARE(importedText.textData.text, text.textData.text);
}


void SvgTests::preservesSvgTransformAndViewBoxOrder()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 100 100" transform="translate(5 0)">
      <rect x="1" y="2" width="10" height="10" transform="translate(10 0) scale(2)"/>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Transforms"), &result, &error), qPrintable(error));
    QCOMPARE(result.layers.size(), 1);
    const LayerNode &root = result.layers.constFirst();
    QCOMPARE(root.children.size(), 1);
    const LayerNode &rectangle = root.children.constFirst();
    // SVG transform lists establish coordinate systems left-to-right, so the
    // local point is scaled before the outer translation. The root viewBox is
    // applied before the root transform, which conceptually sits outside it.
    const QPointF documentPoint = root.transform.map(
        rectangle.transform.map(QPointF(1.0, 2.0)));
    QVERIFY(std::abs(documentPoint.x() - 67.0) < 1.0e-6);
    QVERIFY(std::abs(documentPoint.y() - 4.0) < 1.0e-6);
}

void SvgTests::usesStandaloneSvgDefaultViewport()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg"><rect width="10" height="10"/></svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Defaults"), &result, &error), qPrintable(error));
    QCOMPARE(result.canvasSize, QSize(300, 150));
}


void SvgTests::skipsSingularLocalTransforms()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <rect id="visible" x="1" y="1" width="10" height="10"/>
      <rect id="collapsed" x="20" y="20" width="10" height="10" transform="scale(0)"/>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Singular"), &result, &error), qPrintable(error));
    QCOMPARE(result.layers.size(), 1);
    QCOMPARE(result.layers.constFirst().children.size(), 1);
    QVERIFY(result.skippedElementCount >= 1);
}


void SvgTests::preservesVisibilityOverridesInsideHiddenGroups()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="40">
      <g id="container" visibility="hidden">
        <rect id="hidden" x="0" y="0" width="10" height="10"/>
        <rect id="shown" x="20" y="0" width="10" height="10" visibility="visible"/>
      </g>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Visibility"), &result, &error), qPrintable(error));
    const LayerNode &group = result.layers.constFirst().children.constFirst();
    QCOMPARE(group.type, LayerType::Group);
    QVERIFY(group.visible);
    QCOMPARE(group.children.size(), 2);
    const auto hidden = std::find_if(group.children.cbegin(), group.children.cend(),
                                     [](const LayerNode &layer) { return layer.name == QStringLiteral("hidden"); });
    const auto shown = std::find_if(group.children.cbegin(), group.children.cend(),
                                    [](const LayerNode &layer) { return layer.name == QStringLiteral("shown"); });
    QVERIFY(hidden != group.children.cend());
    QVERIFY(shown != group.children.cend());
    QVERIFY(!hidden->visible);
    QVERIFY(shown->visible);
}


void SvgTests::importsSupportedElementBlendModes()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="40">
      <rect id="blend" x="1" y="1" width="20" height="10" style="mix-blend-mode:multiply"/>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Blend"), &result, &error), qPrintable(error));
    const LayerNode &layer = result.layers.constFirst().children.constFirst();
    QCOMPARE(layer.blendMode, BlendMode::Multiply);
}

void SvgTests::skipsSymbolDefinitionsWithoutUseSupport()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="80" height="40">
      <symbol id="definition"><rect width="30" height="30"/></symbol>
      <rect id="visible" x="1" y="1" width="10" height="10"/>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Symbols"), &result, &error), qPrintable(error));
    const LayerNode &root = result.layers.constFirst();
    QCOMPARE(root.children.size(), 1);
    QCOMPARE(root.children.constFirst().name, QStringLiteral("visible"));
    QVERIFY(result.skippedElementCount >= 1);
    QVERIFY(std::any_of(result.warnings.cbegin(), result.warnings.cend(),
                        [](const QString &warning) { return warning.contains(QStringLiteral("symbol"), Qt::CaseInsensitive); }));
}

void SvgTests::handlesInvisibleAndStrokeOnlyTextHonestly()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="160" height="80">
      <text id="invisible" x="10" y="20" fill="none">Gone</text>
      <text id="outlined" x="10" y="50" fill="none" stroke="#336699">Kept</text>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Text"), &result, &error), qPrintable(error));
    const LayerNode &root = result.layers.constFirst();
    QCOMPARE(root.children.size(), 1);
    const LayerNode &text = root.children.constFirst();
    QCOMPARE(text.type, LayerType::Text);
    QCOMPARE(text.name, QStringLiteral("outlined"));
    QCOMPARE(text.textData.colour.toRgb(), QColor(QStringLiteral("#336699")).toRgb());
    QVERIFY(result.skippedElementCount >= 1);
}

void SvgTests::damagedMetadataFallsBackToStandardGeometry()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="60">
      <g id="fallback" data-vfx-vector-data="not-valid-base64">
        <rect id="geometry" x="5" y="6" width="20" height="12"/>
      </g>
    </svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Fallback"), &result, &error), qPrintable(error));
    const LayerNode &fallback = result.layers.constFirst().children.constFirst();
    QCOMPARE(fallback.type, LayerType::Group);
    QCOMPARE(fallback.children.size(), 1);
    QCOMPARE(fallback.children.constFirst().type, LayerType::Vector);
    QVERIFY(std::any_of(result.warnings.cbegin(), result.warnings.cend(),
                        [](const QString &warning) { return warning.contains(QStringLiteral("metadata"), Qt::CaseInsensitive); }));
}

void SvgTests::selectedNestedExportKeepsWorldTransform()
{
    LayerNode selected;
    selected.type = LayerType::Vector;
    selected.name = QStringLiteral("Selected");
    selected.transform.translate(5.0, 2.0);
    VectorShape rectangle;
    rectangle.type = VectorShapeType::Rectangle;
    rectangle.bounds = QRectF(0.0, 0.0, 20.0, 10.0);
    rectangle.normalise();
    selected.vectorData.objects = {rectangle};
    selected.vectorData.normalise();

    LayerNode sibling = selected;
    sibling.id = QUuid::createUuid();
    sibling.name = QStringLiteral("Sibling");
    sibling.transform.reset();
    for (VectorShape &shape : sibling.vectorData.objects) shape.id = QUuid::createUuid();

    LayerNode group;
    group.type = LayerType::Group;
    group.name = QStringLiteral("Parent");
    group.transform.translate(10.0, 20.0);
    group.children = {sibling, selected};

    SvgExportResult exported;
    QString error;
    const QByteArray svg = SvgWorkflow::exportData(QSize(200, 120), {group}, {selected.id},
                                                   &exported, &error);
    QVERIFY2(!svg.isEmpty(), qPrintable(error));
    QVERIFY(!svg.contains("Sibling"));

    SvgImportResult imported;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Selected"), &imported, &error), qPrintable(error));
    const LayerNode &roundTripped = imported.layers.constFirst().children.constFirst();
    QCOMPARE(roundTripped.type, LayerType::Vector);
    const QPointF mapped = roundTripped.transform.map(QPointF(0.0, 0.0));
    QVERIFY(std::abs(mapped.x() - 15.0) < 1.0e-6);
    QVERIFY(std::abs(mapped.y() - 22.0) < 1.0e-6);
}

void SvgTests::closedPathExportDeclaresFillRule()
{
    LayerNode layer;
    layer.type = LayerType::Vector;
    VectorShape path;
    path.type = VectorShapeType::Path;
    path.bezierPath.closed = true;
    for (const QPointF point : {QPointF(0.0, 0.0), QPointF(20.0, 0.0), QPointF(10.0, 20.0)}) {
        VectorPathNode node;
        node.anchor = point;
        node.clearHandles();
        path.bezierPath.nodes.push_back(node);
    }
    path.bezierPath.normalise();
    path.fill.enabled = true;
    path.normalise();
    layer.vectorData.objects = {path};
    layer.vectorData.normalise();

    SvgExportResult exported;
    QString error;
    const QByteArray svg = SvgWorkflow::exportData(QSize(40, 40), {layer}, {}, &exported, &error);
    QVERIFY2(!svg.isEmpty(), qPrintable(error));
    QVERIFY(svg.contains("fill-rule=\"evenodd\""));
}


void SvgTests::liveCornerExportUsesVisibleGeometryAndExactMetadata()
{
    LayerNode layer;
    layer.type = LayerType::Vector;
    layer.name = QStringLiteral("Live corners");
    VectorShape shape;
    shape.type = VectorShapeType::Path;
    shape.bezierPath.closed = true;
    for (const QPointF anchor : {QPointF(4.0, 4.0), QPointF(44.0, 4.0),
                                 QPointF(44.0, 36.0), QPointF(4.0, 36.0)}) {
        VectorPathNode node;
        node.anchor = anchor;
        node.clearHandles();
        shape.bezierPath.nodes.push_back(node);
    }
    shape.bezierPath.nodes[0].cornerRadius = 8.0;
    shape.bezierPath.nodes[0].cornerStyle = VectorCornerStyle::Rounded;
    shape.bezierPath.nodes[1].cornerRadius = 6.0;
    shape.bezierPath.nodes[1].cornerStyle = VectorCornerStyle::Chamfer;
    shape.bezierPath.normalise();
    shape.fill.enabled = true;
    shape.fill.colour = QColor(60, 150, 220);
    shape.normalise();
    layer.vectorData.objects = {shape};
    layer.vectorData.normalise();

    SvgExportResult exported;
    QString error;
    const QByteArray svg = SvgWorkflow::exportData(QSize(48, 40), {layer}, {},
                                                   &exported, &error);
    QVERIFY2(!svg.isEmpty(), qPrintable(error));
    QVERIFY(svg.contains("data-vfx-vector-data="));
    QVERIFY(svg.contains(" C ") || svg.contains("C "));

    SvgImportResult imported;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Live corners"),
                                     &imported, &error), qPrintable(error));
    QCOMPARE(imported.layers.size(), 1);
    QCOMPARE(imported.layers.constFirst().children.size(), 1);
    const VectorShape roundTripped = imported.layers.constFirst().children.constFirst()
                                         .vectorData.objects.constFirst();
    QCOMPARE(roundTripped.type, VectorShapeType::Path);
    QCOMPARE(roundTripped.bezierPath, shape.bezierPath);
    QVERIFY(roundTripped.bezierPath.hasLiveCorners());
}


void SvgTests::dashedStrokeImportsExportsAndPreservesCaps()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="80">
      <path id="dash" d="M 10 40 L 230 40" fill="none" stroke="#2455cc"
            stroke-width="6" stroke-linecap="round"
            stroke-dasharray="18 7" stroke-dashoffset="3"/>
    </svg>)SVG";
    SvgImportResult imported;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Dashed"), &imported, &error),
             qPrintable(error));

    VectorShape importedShape;
    bool found = false;
    std::function<void(const QVector<LayerNode> &)> findShape;
    findShape = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (!found && layer.type == LayerType::Vector
                && !layer.vectorData.objects.isEmpty()) {
                importedShape = layer.vectorData.objects.constFirst();
                found = true;
                return;
            }
            findShape(layer.children);
            if (found) return;
        }
    };
    findShape(imported.layers);
    QVERIFY(found);
    QCOMPARE(importedShape.stroke.pattern, VectorStrokePattern::Dashed);
    QCOMPARE(importedShape.stroke.dashLength, 18.0);
    QCOMPARE(importedShape.stroke.gapLength, 7.0);
    QCOMPARE(importedShape.stroke.dashOffset, 3.0);
    QCOMPARE(importedShape.stroke.cap, VectorStrokeCap::Round);

    SvgExportResult exported;
    const QByteArray encoded = SvgWorkflow::exportData(
        imported.canvasSize, imported.layers, {}, &exported, &error);
    QVERIFY2(!encoded.isEmpty(), qPrintable(error));
    QVERIFY(encoded.contains("stroke-dasharray=\"18 7\""));
    QVERIFY(encoded.contains("stroke-dashoffset=\"3\""));
    QVERIFY(encoded.contains("stroke-linecap=\"round\""));

    SvgImportResult roundTripped;
    QVERIFY2(SvgWorkflow::importData(encoded, QStringLiteral("Dashed round trip"),
                                     &roundTripped, &error), qPrintable(error));
    VectorShape roundTripShape;
    found = false;
    findShape = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (!found && layer.type == LayerType::Vector
                && !layer.vectorData.objects.isEmpty()) {
                roundTripShape = layer.vectorData.objects.constFirst();
                found = true;
                return;
            }
            findShape(layer.children);
            if (found) return;
        }
    };
    findShape(roundTripped.layers);
    QVERIFY(found);
    QCOMPARE(roundTripShape.stroke, importedShape.stroke);
}



void SvgTests::arrowheadsAndArrowShapeRoundTripWithSvgMarkers()
{
    LayerNode lineLayer;
    lineLayer.type = LayerType::Vector;
    lineLayer.name = QStringLiteral("Arrowed line");
    VectorShape line;
    line.type = VectorShapeType::Line;
    line.lineStart = QPointF(24.0, 48.0);
    line.lineEnd = QPointF(210.0, 78.0);
    line.fill.enabled = false;
    line.stroke.enabled = true;
    line.stroke.colour = QColor(32, 89, 210, 220);
    line.stroke.width = 7.0;
    line.stroke.startArrowhead = VectorArrowheadType::Circle;
    line.stroke.endArrowhead = VectorArrowheadType::Stealth;
    line.stroke.startArrowScale = 1.15;
    line.stroke.endArrowScale = 1.7;
    line.normalise();
    lineLayer.vectorData.objects = {line};
    lineLayer.vectorData.normalise();

    LayerNode arrowLayer;
    arrowLayer.type = LayerType::Vector;
    arrowLayer.name = QStringLiteral("Block arrow");
    VectorShape arrow;
    arrow.type = VectorShapeType::Arrow;
    arrow.bounds = QRectF(40.0, 100.0, 150.0, 70.0);
    arrow.arrowHeadLengthRatio = 0.41;
    arrow.arrowShaftWidthRatio = 0.3;
    arrow.fill.enabled = true;
    arrow.fill.colour = QColor(238, 130, 35, 206);
    arrow.stroke.enabled = true;
    arrow.stroke.colour = QColor(60, 45, 30);
    arrow.stroke.width = 3.0;
    arrow.normalise();
    arrowLayer.vectorData.objects = {arrow};
    arrowLayer.vectorData.normalise();

    SvgExportResult exported;
    QString error;
    const QByteArray svg = SvgWorkflow::exportData(
        QSize(260, 200), {lineLayer, arrowLayer}, {}, &exported, &error);
    QVERIFY2(!svg.isEmpty(), qPrintable(error));
    QVERIFY2(svg.contains("<marker"), svg.constData());
    QVERIFY2(svg.contains("marker-start=\"url(#vfx-arrow-start-circle-1150)\""),
             svg.constData());
    QVERIFY2(svg.contains("marker-end=\"url(#vfx-arrow-end-stealth-1700)\""),
             svg.constData());
    QVERIFY2(svg.contains("data-vfx-start-arrowhead=\"circle\""), svg.constData());
    QVERIFY2(svg.contains("data-vfx-end-arrowhead=\"stealth\""), svg.constData());
    QVERIFY2(svg.contains("<circle cx=\"0\" cy=\"0\" r=\"1.5\""), svg.constData());
    QVERIFY2(!svg.contains("<circle cx=\"-1.5\""), svg.constData());
    QVERIFY2(svg.contains("data-vfx-shape-type=\"arrow\""), svg.constData());
    QVERIFY2(svg.contains("data-vfx-arrow-head-length=\"0.41\""), svg.constData());

    SvgImportResult imported;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Arrow round trip"),
                                     &imported, &error), qPrintable(error));
    QVector<VectorShape> roundTripShapes;
    std::function<void(const QVector<LayerNode> &)> collect;
    collect = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (layer.type == LayerType::Vector) {
                for (const VectorShape &shape : layer.vectorData.objects) {
                    roundTripShapes.push_back(shape);
                }
            }
            collect(layer.children);
        }
    };
    collect(imported.layers);
    QCOMPARE(roundTripShapes.size(), 2);
    const auto lineIt = std::find_if(roundTripShapes.cbegin(), roundTripShapes.cend(),
                                     [](const VectorShape &shape) {
                                         return shape.type == VectorShapeType::Line;
                                     });
    const auto arrowIt = std::find_if(roundTripShapes.cbegin(), roundTripShapes.cend(),
                                      [](const VectorShape &shape) {
                                          return shape.type == VectorShapeType::Arrow;
                                      });
    QVERIFY(lineIt != roundTripShapes.cend());
    QVERIFY(arrowIt != roundTripShapes.cend());
    QCOMPARE(lineIt->stroke.startArrowhead, VectorArrowheadType::Circle);
    QCOMPARE(lineIt->stroke.endArrowhead, VectorArrowheadType::Stealth);
    QCOMPARE(lineIt->stroke.startArrowScale, 1.15);
    QCOMPARE(lineIt->stroke.endArrowScale, 1.7);
    QCOMPARE(arrowIt->arrowHeadLengthRatio, 0.41);
    QCOMPARE(arrowIt->arrowShaftWidthRatio, 0.3);
}

void SvgTests::compoundPathImportsExportsWithoutConnectorGeometry()
{
    const QByteArray svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
      <path id="ring" fill="#4d55a4" fill-rule="evenodd"
            d="M 20 20 L 180 20 L 180 100 L 20 100 Z M 55 45 L 145 45 L 145 75 L 55 75 Z"/>
    </svg>)SVG";

    SvgImportResult imported;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Compound Ring"),
                                     &imported, &error), qPrintable(error));

    VectorShape importedShape;
    bool found = false;
    std::function<void(const QVector<LayerNode> &)> findShape;
    findShape = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (!found && layer.type == LayerType::Vector
                && !layer.vectorData.objects.isEmpty()) {
                importedShape = layer.vectorData.objects.constFirst();
                found = true;
            }
            if (!found) findShape(layer.children);
        }
    };
    findShape(imported.layers);
    QVERIFY(found);
    QCOMPARE(importedShape.type, VectorShapeType::Path);
    QVERIFY(importedShape.bezierPath.closed);
    QCOMPARE(importedShape.additionalBezierPaths.size(), 1);
    QVERIFY(importedShape.additionalBezierPaths.constFirst().closed);
    QVERIFY(importedShape.geometryPath().contains(QPointF(30.0, 30.0)));
    QVERIFY(!importedShape.geometryPath().contains(QPointF(80.0, 60.0)));

    SvgExportResult exportResult;
    const QByteArray exported = SvgWorkflow::exportData(
        imported.canvasSize, imported.layers, {}, &exportResult, &error);
    QVERIFY2(!exported.isEmpty(), qPrintable(error));
    QVERIFY2(exported.contains("fill-rule=\"evenodd\""), exported.constData());
    QVERIFY2(exported.contains("Z M"),
             "Each compound contour must close before the next subpath begins; connector lines are forbidden.");

    SvgImportResult roundTripped;
    QVERIFY2(SvgWorkflow::importData(exported, QStringLiteral("Compound Round Trip"),
                                     &roundTripped, &error), qPrintable(error));
    VectorShape roundTripShape;
    found = false;
    findShape = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (!found && layer.type == LayerType::Vector
                && !layer.vectorData.objects.isEmpty()) {
                roundTripShape = layer.vectorData.objects.constFirst();
                found = true;
            }
            if (!found) findShape(layer.children);
        }
    };
    findShape(roundTripped.layers);
    QVERIFY(found);
    QCOMPARE(roundTripShape.type, VectorShapeType::Path);
    QCOMPARE(roundTripShape.additionalBezierPaths.size(), 1);
    QVERIFY(roundTripShape.geometryPath().contains(QPointF(30.0, 30.0)));
    QVERIFY(!roundTripShape.geometryPath().contains(QPointF(80.0, 60.0)));

    const QByteArray nonzeroSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
      <path id="winding-ring" fill="#4d55a4" fill-rule="nonzero"
            d="M 20 20 L 180 20 L 180 100 L 20 100 Z M 55 45 L 55 75 L 145 75 L 145 45 Z"/>
    </svg>)SVG";
    SvgImportResult nonzeroImported;
    QVERIFY2(SvgWorkflow::importData(nonzeroSvg, QStringLiteral("Nonzero Ring"),
                                     &nonzeroImported, &error), qPrintable(error));
    VectorShape nonzeroShape;
    found = false;
    findShape = [&](const QVector<LayerNode> &layers) {
        for (const LayerNode &layer : layers) {
            if (!found && layer.type == LayerType::Vector
                && !layer.vectorData.objects.isEmpty()) {
                nonzeroShape = layer.vectorData.objects.constFirst();
                found = true;
            }
            if (!found) findShape(layer.children);
        }
    };
    findShape(nonzeroImported.layers);
    QVERIFY(found);
    QCOMPARE(nonzeroShape.pathFillRule, VectorPathFillRule::NonZero);
    QCOMPARE(nonzeroShape.additionalBezierPaths.size(), 1);
    QVERIFY(nonzeroShape.geometryPath().contains(QPointF(30.0, 30.0)));
    QVERIFY(!nonzeroShape.geometryPath().contains(QPointF(80.0, 60.0)));

    const QByteArray nonzeroExported = SvgWorkflow::exportData(
        nonzeroImported.canvasSize, nonzeroImported.layers, {},
        &exportResult, &error);
    QVERIFY2(!nonzeroExported.isEmpty(), qPrintable(error));
    QVERIFY2(nonzeroExported.contains("fill-rule=\"nonzero\""),
             nonzeroExported.constData());
}


void SvgTests::rejectsMalformedArcFlagsWithoutLosingSiblings()
{
    const QByteArray legal = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">
      <path d="M 2 20 A 12 12 0 0 1 30 20" fill="none" stroke="#ffffff"/>
    </svg>)SVG";
    SvgImportResult legalResult;
    QString error;
    QVERIFY2(SvgWorkflow::importData(legal, QStringLiteral("Legal Arc"), &legalResult, &error),
             qPrintable(error));
    QCOMPARE(legalResult.layers.constFirst().children.size(), 1);

    const QByteArray malformed = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">
      <path d="M 2 20 A 12 12 0 2 1 30 20" fill="none" stroke="#ffffff"/>
      <rect x="4" y="4" width="8" height="8" fill="#ff0000"/>
    </svg>)SVG";
    SvgImportResult malformedResult;
    error.clear();
    QVERIFY2(SvgWorkflow::importData(malformed, QStringLiteral("Malformed Arc"),
                                     &malformedResult, &error), qPrintable(error));
    QCOMPARE(malformedResult.layers.constFirst().children.size(), 1);
    QVERIFY(malformedResult.skippedElementCount >= 1);
    QVERIFY(!malformedResult.warnings.isEmpty());
}

void SvgTests::boundsCumulativeImportAndExportComplexity()
{
    QByteArray svg("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\">");
    svg.reserve(1024 * 1024);
    for (int index = 0; index < SvgWorkflow::MaximumEditableLayerCount + 1; ++index) {
        svg += "<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\"/>";
    }
    svg += "</svg>";

    SvgImportResult imported;
    QString error;
    QVERIFY2(SvgWorkflow::importData(svg, QStringLiteral("Bounded"), &imported, &error),
             qPrintable(error));
    QCOMPARE(imported.layers.constFirst().children.size(), SvgWorkflow::MaximumEditableLayerCount);
    QVERIFY(imported.skippedElementCount >= 1);

    LayerNode vector;
    vector.type = LayerType::Vector;
    VectorShape rectangle;
    rectangle.type = VectorShapeType::Rectangle;
    rectangle.bounds = QRectF(0.0, 0.0, 1.0, 1.0);
    rectangle.normalise();
    vector.vectorData.objects = {rectangle};
    vector.vectorData.normalise();

    QVector<LayerNode> layers;
    layers.reserve(SvgWorkflow::MaximumEditableLayerCount + 1);
    for (int index = 0; index < SvgWorkflow::MaximumEditableLayerCount + 1; ++index) {
        LayerNode copy = vector;
        copy.id = QUuid::createUuid();
        layers.push_back(std::move(copy));
    }
    SvgExportResult exported;
    error.clear();
    QVERIFY(SvgWorkflow::exportData(QSize(64, 64), layers, {}, &exported, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void SvgTests::rejectsUnsafeExportHierarchy()
{
    LayerNode vector;
    vector.type = LayerType::Vector;
    VectorShape rectangle;
    rectangle.type = VectorShapeType::Rectangle;
    rectangle.bounds = QRectF(0.0, 0.0, 10.0, 10.0);
    rectangle.normalise();
    vector.vectorData.objects = {rectangle};
    vector.vectorData.normalise();

    LayerNode group;
    group.type = LayerType::Group;
    group.transform.scale(0.0, 0.0);
    group.children = {vector};

    SvgExportResult exported;
    QString error;
    QVERIFY(SvgWorkflow::exportData(QSize(40, 40), {group}, {}, &exported, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void SvgTests::rejectsEntityDeclarations()
{
    const QByteArray svg = R"SVG(<!DOCTYPE svg [<!ENTITY xxe "bad">]><svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><text>&xxe;</text></svg>)SVG";
    SvgImportResult result;
    QString error;
    QVERIFY(!SvgWorkflow::importData(svg, QStringLiteral("Unsafe"), &result, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(SvgTests)
#include "test_svg.moc"
