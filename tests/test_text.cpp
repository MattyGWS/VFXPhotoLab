#include "TextLayer.h"
#include "TextRasterizer.h"

#include <QColorSpace>
#include <QImage>
#include <QtTest/QtTest>

#include <algorithm>

using namespace vfx;

class TextTests final : public QObject {
    Q_OBJECT

private slots:
    void pointTextMeasuresExplicitNewlines();
    void autoHeightGrowsForWrappedAndExplicitLines();
    void fittedFontSizeShrinksOnlyEnoughToFit();
    void rasterizerDrawsEveryExplicitLine();
};

void TextTests::pointTextMeasuresExplicitNewlines()
{
    TextLayerData data;
    data.mode = TextLayoutMode::Point;
    data.fontSize = 24.0;
    data.leading = 1.2;
    data.text = QStringLiteral("One line");
    data.normalise();
    const QRectF single = data.semanticBox();

    data.text = QStringLiteral("One line\nSecond line\nThird line");
    const QRectF multiple = data.semanticBox();

    QVERIFY(single.width() > 1.0);
    QVERIFY(single.height() > 1.0);
    QVERIFY(multiple.height() > single.height() * 2.5);
    QVERIFY(multiple.width() >= single.width() * 0.5);
}

void TextTests::autoHeightGrowsForWrappedAndExplicitLines()
{
    TextLayerData data;
    data.mode = TextLayoutMode::Area;
    data.overflow = TextOverflowMode::AutoHeight;
    data.area = QRectF(20.0, 30.0, 110.0, 8.0);
    data.fontSize = 22.0;
    data.leading = 1.25;
    data.text = QStringLiteral(
        "A deliberately long first line that must wrap.\n"
        "A second explicit line.");
    data.normalise();

    const double required = data.requiredHeight(data.area.width());
    QVERIFY(required > data.area.height());
    data.growBoxToFit();

    QVERIFY(data.area.height() >= required - 0.01);
    QCOMPARE(data.semanticBox().topLeft(), data.area.topLeft());
    QVERIFY(data.semanticBox().height() >= required - 0.01);
}

void TextTests::fittedFontSizeShrinksOnlyEnoughToFit()
{
    TextLayerData data;
    data.mode = TextLayoutMode::Area;
    data.overflow = TextOverflowMode::Clip;
    data.area = QRectF(0.0, 0.0, 180.0, 160.0);
    data.fontSize = 64.0;
    data.leading = 1.2;
    data.text = QStringLiteral("First line\nSecond line\nThird line");
    data.normalise();

    const double fullHeight = data.requiredHeight(data.area.width(), data.fontSize);
    const double targetHeight = fullHeight * 0.62;
    const double fitted = data.fittedFontSize(
        data.area.width(), targetHeight, data.fontSize);

    QVERIFY(fitted >= 1.0);
    QVERIFY(fitted < data.fontSize);
    QVERIFY(data.requiredHeight(data.area.width(), fitted)
            <= targetHeight + 1.0);

    const double generous = data.fittedFontSize(
        data.area.width(), fullHeight * 1.5, data.fontSize);
    QCOMPARE(generous, data.fontSize);
}

void TextTests::rasterizerDrawsEveryExplicitLine()
{
    LayerNode layer;
    layer.type = LayerType::Text;
    layer.textData.mode = TextLayoutMode::Point;
    layer.textData.origin = QPointF(12.0, 10.0);
    layer.textData.text = QStringLiteral("Top\nBottom");
    layer.textData.fontSize = 26.0;
    layer.textData.leading = 1.25;
    layer.textData.colour = QColor(20, 180, 70, 255);
    layer.textData.normalise();

    const QSize size(220, 120);
    const QImage rendered = TextRasterizer::renderLayerRegion(
        layer, size, QRect(QPoint(0, 0), size), size, QTransform(),
        QImage::Format_RGBA8888, QColorSpace(QColorSpace::SRgb),
        false, false, nullptr);

    QVERIFY(!rendered.isNull());
    int firstRow = rendered.height();
    int lastRow = -1;
    for (int y = 0; y < rendered.height(); ++y) {
        bool populated = false;
        for (int x = 0; x < rendered.width(); ++x) {
            if (rendered.pixelColor(x, y).alpha() > 0) {
                populated = true;
                break;
            }
        }
        if (populated) {
            firstRow = std::min(firstRow, y);
            lastRow = std::max(lastRow, y);
        }
    }

    QVERIFY(lastRow >= firstRow);
    QVERIFY(lastRow - firstRow > 26);
}

QTEST_MAIN(TextTests)

#include "test_text.moc"
