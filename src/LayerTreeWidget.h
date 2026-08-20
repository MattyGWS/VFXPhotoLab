#pragma once

#include <QItemSelectionModel>
#include <QTreeWidget>
#include <QUuid>
#include <QVector>

class QEvent;
class QMouseEvent;

namespace vfx {

class LayerTreeWidget final : public QTreeWidget {
    Q_OBJECT

public:
    explicit LayerTreeWidget(QWidget *parent = nullptr);

    QList<QTreeWidgetItem *> selectedRootItems() const;

signals:
    // The document model remains the source of truth. Drop requests describe
    // the desired move and MainWindow applies it to PhotoDocument before the
    // tree is rebuilt. This avoids QTreeWidget's deferred internal-move state.
    void layersDropRequested(const QVector<QUuid> &layerIds,
                             const QUuid &destinationParentId,
                             int destinationIndex);
    void structureChanged();
    // Ctrl-clicking a thumbnail loads its coverage as the document selection
    // without toggling the Layers tree selection itself.
    void thumbnailSelectionRequested(const QUuid &layerId, bool maskThumbnail);

protected:
    QItemSelectionModel::SelectionFlags selectionCommand(
        const QModelIndex &index, const QEvent *event = nullptr) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dropEvent(QDropEvent *event) override;
    void drawBranches(QPainter *painter,
                      const QRect &rect,
                      const QModelIndex &index) const override;

public:
    bool moveSelectionUp();
    bool moveSelectionDown();

private:
    QList<QTreeWidgetItem *> m_dragSources;
};

} // namespace vfx
