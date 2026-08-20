#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPainterPath>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace vfx {

enum class SelectionShape {
    Rectangle,
    Ellipse
};

enum class SelectionCombineMode {
    Replace,
    Add,
    Subtract,
    Intersect
};

// One document-space pixel selection. Coverage is always 8-bit, independent of
// the document's RGB bit depth. The implicit value represents every tile not
// present in m_tiles, so Select All and active-empty selections require no
// image-sized allocation.
class SelectionMask final {
public:
    static constexpr int TileSize = 256;

    struct Snapshot {
        QSize size;
        bool active = false;
        quint8 implicitCoverage = 0;
        QHash<quint64, QByteArray> tiles;
        QRect nonZeroBounds;
        quint64 revision = 0;
    };

    SelectionMask() = default;
    explicit SelectionMask(const QSize &documentSize);

    void reset(const QSize &documentSize);
    const QSize &size() const;
    bool isActive() const;
    bool isEmpty() const;
    bool isFull() const;
    quint8 implicitCoverage() const;
    quint64 revision() const;
    int explicitTileCount() const;
    qint64 estimatedResidentBytes() const;
    QRect nonZeroBounds() const;

    void deactivate();
    void selectAll();
    void selectNone();

    quint8 coverageAt(int x, int y) const;
    quint8 coverageAt(const QPoint &position) const;
    QImage coverageImage(const QRect &documentRect = {},
                         const QSize &outputSize = {}) const;

    // Document-space mutation helpers shared by selection tools, source
    // commands and tests. One call advances the selection revision once.
    bool setCoverageRect(const QRect &documentRect, quint8 coverage);
    bool setCoverageImage(const QRect &documentRect, const QImage &coverageImage);
    bool combineShape(const QRectF &documentBounds,
                      SelectionShape shape,
                      SelectionCombineMode mode,
                      bool antialias);
    bool combinePath(const QPainterPath &documentPath,
                     SelectionCombineMode mode,
                     bool antialias);
    // Combine arbitrary 8-bit document-space coverage. Pixels outside
    // documentRect are treated as zero for Replace and Intersect, while Add
    // and Subtract preserve the current selection outside the source region.
    bool combineCoverageImage(const QRect &documentRect,
                              const QImage &coverageImage,
                              SelectionCombineMode mode);

    Snapshot snapshot() const;
    bool restoreSnapshot(const Snapshot &snapshot, bool advanceRevision = true);

    QVector<QPoint> explicitTileIndices() const;
    QByteArray actualTileBytes(const QPoint &tileIndex) const;
    QSize tilePixelSize(const QPoint &tileIndex) const;

    QJsonObject toJson(bool *ok = nullptr) const;
    static SelectionMask fromJson(const QJsonObject &object,
                                  const QSize &documentSize,
                                  bool *ok = nullptr,
                                  QString *warning = nullptr);

    static quint64 tileKey(const QPoint &tileIndex);
    static QPoint tileIndexFromKey(quint64 key);

private:
    friend struct SelectionTileDeltaSet;

    bool validDocumentPosition(int x, int y) const;
    bool validTileIndex(const QPoint &tileIndex) const;
    QByteArray canonicalTileBytes(const QPoint &tileIndex,
                                  const QByteArray &actualBytes,
                                  quint8 implicitCoverage) const;
    void setActualTileBytes(const QPoint &tileIndex, const QByteArray &actualBytes);
    void recalculateBounds();
    void normaliseStorage();
    void touch();

    QSize m_size;
    bool m_active = false;
    quint8 m_implicitCoverage = 0;
    QHash<quint64, QByteArray> m_tiles;
    QRect m_nonZeroBounds;
    quint64 m_revision = 1;
};

} // namespace vfx
