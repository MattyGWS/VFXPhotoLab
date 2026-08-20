#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <Qt>
#include <QtGlobal>

namespace vfx {

enum class TextLayoutMode { Point, Area };
enum class TextOverflowMode { AutoHeight, Clip };

QString textLayoutModeToString(TextLayoutMode mode);
TextLayoutMode textLayoutModeFromString(const QString &value, bool *ok = nullptr);
QString textOverflowModeToString(TextOverflowMode mode);
TextOverflowMode textOverflowModeFromString(const QString &value, bool *ok = nullptr);

struct TextLayerData {
    static constexpr quint32 CurrentSchema = 1;
    static constexpr int MaximumTextLength = 1'000'000;

    quint32 schema = CurrentSchema;
    QString text = QStringLiteral("Text");
    TextLayoutMode mode = TextLayoutMode::Point;
    QPointF origin {0.0, 0.0};
    QRectF area {0.0, 0.0, 320.0, 120.0};
    QString requestedFamily;
    QString requestedStyle;
    int weight = 400;
    bool italic = false;
    double fontSize = 32.0;
    QColor colour = QColor(Qt::black);
    double opacity = 1.0;
    Qt::Alignment horizontalAlignment = Qt::AlignLeft;
    double tracking = 0.0;
    double leading = 1.2;
    TextOverflowMode overflow = TextOverflowMode::AutoHeight;
    quint64 revision = 1;

    void normalise();
    bool isSafe() const;
    QString resolvedFamily(bool *missing = nullptr) const;
    QRectF semanticBox() const;
    double requiredHeight(double width = -1.0, double fontSizeOverride = -1.0) const;
    double fittedFontSize(double width, double height, double maximumFontSize) const;
    void growBoxToFit();
    QRectF localBounds() const;
    QRectF contentBounds(const QTransform &worldTransform = QTransform()) const;
    quint64 fingerprint() const;
    qint64 estimatedBytes() const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static TextLayerData fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const TextLayerData &) const = default;
};

} // namespace vfx
