#include "TextRasterizer.h"
#include "TextLayer.h"
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextLayout>
#include <QTextOption>
#include <QStringList>
#include <algorithm>

namespace vfx {
namespace {
QFont fontFor(const TextLayerData &d)
{
    const QString family = d.resolvedFamily();
    QFont font = d.requestedStyle.isEmpty() ? QFont(family)
        : QFontDatabase::font(family, d.requestedStyle, qRound(d.fontSize));
    font.setPixelSize(std::max(1, qRound(d.fontSize)));
    font.setWeight(static_cast<QFont::Weight>(std::clamp(d.weight, 1, 1000)));
    font.setItalic(d.italic);
    font.setLetterSpacing(QFont::AbsoluteSpacing, d.tracking);
    return font;
}

void drawTextLayout(QPainter &painter, const TextLayerData &data)
{
    const QFontMetricsF metrics(painter.font());
    const double lineAdvance = std::max(metrics.height(), data.fontSize * data.leading);
    const QStringList paragraphs = data.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const QRectF box = data.mode == TextLayoutMode::Point
        ? data.semanticBox() : data.area;
    const double width = std::max(1.0, box.width());
    double y = box.top();

    if (data.mode == TextLayoutMode::Area
        && data.overflow == TextOverflowMode::Clip) {
        painter.setClipRect(data.area, Qt::IntersectClip);
    }

    for (const QString &paragraph : paragraphs) {
        if (paragraph.isEmpty()) {
            y += lineAdvance;
            if (data.mode == TextLayoutMode::Area
                && data.overflow == TextOverflowMode::Clip
                && y > data.area.bottom()) break;
            continue;
        }
        QTextLayout layout(paragraph, painter.font());
        QTextOption option;
        option.setWrapMode(data.mode == TextLayoutMode::Area
            ? QTextOption::WrapAtWordBoundaryOrAnywhere
            : QTextOption::NoWrap);
        option.setAlignment(data.horizontalAlignment);
        layout.setTextOption(option);
        layout.beginLayout();
        int guard = 0;
        while (++guard < 100000) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            line.setPosition(QPointF(box.left(), y));
            line.draw(&painter, QPointF());
            y += std::max(line.height(), lineAdvance);
            if (data.mode == TextLayoutMode::Area
                && data.overflow == TextOverflowMode::Clip
                && y > data.area.bottom()) break;
        }
        layout.endLayout();
        if (data.mode == TextLayoutMode::Area
            && data.overflow == TextOverflowMode::Clip
            && y > data.area.bottom()) break;
    }
}
}
QImage TextRasterizer::renderLayerRegion(const LayerNode &layer, const QSize &sourceSize,
    const QRect &sourceRegion, const QSize &documentSize, const QTransform &worldTransform,
    QImage::Format format, const QColorSpace &colourSpace, bool forceOpaquePixelAlpha,
    bool grayscaleDocument, const std::atomic_bool *cancelRequested)
{
    Q_UNUSED(forceOpaquePixelAlpha)
    if (layer.type != LayerType::Text || !layer.textData.isSafe() || sourceRegion.isEmpty()) return {};
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) return {};
    QImage image(sourceRegion.size(), format);
    image.fill(Qt::transparent); image.setColorSpace(colourSpace);
    QPainter painter(&image); painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    const QTransform documentToPreview = QTransform::fromScale(
        sourceSize.width() / static_cast<double>(std::max(1, documentSize.width())),
        sourceSize.height() / static_cast<double>(std::max(1, documentSize.height())));
    const QTransform documentToTile = documentToPreview
        * QTransform::fromTranslate(-sourceRegion.left(), -sourceRegion.top());
    painter.setWorldTransform(worldTransform * documentToTile);
    QColor colour = layer.textData.colour;
    if (grayscaleDocument) { const int g = qGray(colour.rgb()); colour.setRgb(g,g,g,colour.alpha()); }
    colour.setAlphaF(std::clamp(colour.alphaF() * layer.textData.opacity, 0.0, 1.0));
    painter.setPen(colour); painter.setFont(fontFor(layer.textData));
    painter.save();
    drawTextLayout(painter, layer.textData);
    painter.restore();
    painter.end();
    if (cancelRequested && cancelRequested->load(std::memory_order_acquire)) return {};
    return image;
}
QRectF TextRasterizer::contentBounds(const LayerNode &layer, const QTransform &worldTransform)
{
    return layer.type == LayerType::Text && layer.textData.isSafe() ? layer.textData.contentBounds(worldTransform) : QRectF();
}
}
