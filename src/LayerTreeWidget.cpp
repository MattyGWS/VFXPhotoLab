#include "LayerTreeWidget.h"

#include "Adjustment.h"

#include <QDropEvent>
#include <QItemSelectionModel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QTreeWidgetItemIterator>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace vfx {
namespace {

constexpr int LayerIdRole = Qt::UserRole + 1;
constexpr int LayerTypeRole = Qt::UserRole + 2;
constexpr int LiveFilterIdRole = Qt::UserRole + 3;
constexpr int LayerEffectIdRole = Qt::UserRole + 5;
constexpr int LayerEffectContainerRole = Qt::UserRole + 7;

bool isPresentationOnlyItem(const QTreeWidgetItem *item)
{
    return item
        && (!item->data(0, LiveFilterIdRole).toString().isEmpty()
            || !item->data(0, LayerEffectIdRole).toString().isEmpty()
            || item->data(0, LayerEffectContainerRole).toBool());
}

bool isTreeItemAncestor(const QTreeWidgetItem *possibleAncestor, const QTreeWidgetItem *item)
{
    for (const QTreeWidgetItem *parent = item ? item->parent() : nullptr;
         parent;
         parent = parent->parent()) {
        if (parent == possibleAncestor) {
            return true;
        }
    }
    return false;
}

int itemIndex(const QTreeWidget *tree, const QTreeWidgetItem *item)
{
    return item->parent() ? item->parent()->indexOfChild(const_cast<QTreeWidgetItem *>(item))
                          : tree->indexOfTopLevelItem(const_cast<QTreeWidgetItem *>(item));
}

QTreeWidgetItem *takeItem(QTreeWidget *tree, QTreeWidgetItem *item)
{
    if (QTreeWidgetItem *parent = item->parent()) {
        return parent->takeChild(parent->indexOfChild(item));
    }
    return tree->takeTopLevelItem(tree->indexOfTopLevelItem(item));
}

void insertItem(QTreeWidget *tree,
                QTreeWidgetItem *parent,
                const int index,
                QTreeWidgetItem *item)
{
    if (parent) {
        parent->insertChild(std::clamp(index, 0, parent->childCount()), item);
        parent->setExpanded(true);
    } else {
        tree->insertTopLevelItem(std::clamp(index, 0, tree->topLevelItemCount()), item);
    }
}

bool hasSelectedAncestor(const QTreeWidgetItem *item)
{
    for (const QTreeWidgetItem *parent = item ? item->parent() : nullptr;
         parent;
         parent = parent->parent()) {
        if (parent->isSelected()) {
            return true;
        }
    }
    return false;
}

} // namespace

LayerTreeWidget::LayerTreeWidget(QWidget *parent)
    : QTreeWidget(parent)
{
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(true);
    setItemsExpandable(true);
    setExpandsOnDoubleClick(false);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setAutoScroll(true);
}


QItemSelectionModel::SelectionFlags LayerTreeWidget::selectionCommand(
    const QModelIndex &index, const QEvent *event) const
{
    if (event
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease)
        && index.isValid()) {
        const auto *mouseEvent = static_cast<const QMouseEvent *>(event);
        QTreeWidgetItem *item = itemFromIndex(index);
        if (mouseEvent->button() == Qt::RightButton
            && item && item->isSelected()) {
            // ExtendedSelection normally collapses to the context-clicked row
            // before customContextMenuRequested is emitted. Preserve an
            // existing multi-selection when the click lands on one of its
            // members while allowing QAbstractItemView to keep its ordinary
            // focus, current-index and context-menu event handling.
            return QItemSelectionModel::NoUpdate;
        }
    }
    return QTreeWidget::selectionCommand(index, event);
}

void LayerTreeWidget::mousePressEvent(QMouseEvent *event)
{
    if (event && event->button() == Qt::LeftButton
        && event->modifiers().testFlag(Qt::ControlModifier)) {
        QTreeWidgetItem *item = itemAt(event->position().toPoint());
        const int column = columnAt(event->position().toPoint().x());
        if (item && (column == 1 || column == 2)) {
            const QUuid id(item->data(0, LayerIdRole).toString());
            if (!id.isNull()) {
                emit thumbnailSelectionRequested(id, column == 2);
                event->accept();
                return;
            }
        }
    }
    QTreeWidget::mousePressEvent(event);
}

QList<QTreeWidgetItem *> LayerTreeWidget::selectedRootItems() const
{
    QList<QTreeWidgetItem *> roots;
    for (QTreeWidgetItemIterator iterator(const_cast<LayerTreeWidget *>(this));
         *iterator;
         ++iterator) {
        QTreeWidgetItem *item = *iterator;
        if (item->isSelected() && !hasSelectedAncestor(item)
            && !isPresentationOnlyItem(item)) {
            roots.push_back(item);
        }
    }
    return roots;
}

void LayerTreeWidget::startDrag(const Qt::DropActions supportedActions)
{
    if (isPresentationOnlyItem(currentItem())) {
        m_dragSources.clear();
        return;
    }
    m_dragSources = selectedRootItems();
    if (m_dragSources.isEmpty() && currentItem()) {
        currentItem()->setSelected(true);
        m_dragSources = selectedRootItems();
    }
    // Live Filter and Layer Effect rows are presentation children, not document
    // LayerNodes. Never hand them to QTreeWidget's structural drag machinery.
    for (QTreeWidgetItem *item : std::as_const(m_dragSources)) {
        if (!item || isPresentationOnlyItem(item)) {
            m_dragSources.clear();
            return;
        }
    }
    QTreeWidget::startDrag(supportedActions);
}

void LayerTreeWidget::dropEvent(QDropEvent *event)
{
    QList<QTreeWidgetItem *> sources = m_dragSources;
    if (sources.isEmpty()) {
        sources = selectedRootItems();
    }
    QTreeWidgetItem *target = itemAt(event->position().toPoint());
    if (sources.isEmpty()) {
        event->ignore();
        return;
    }
    if (isPresentationOnlyItem(target)) {
        m_dragSources.clear();
        event->ignore();
        return;
    }
    for (QTreeWidgetItem *source : std::as_const(sources)) {
        if (isPresentationOnlyItem(source)) {
            m_dragSources.clear();
            event->ignore();
            return;
        }
    }

    for (QTreeWidgetItem *source : sources) {
        if (!source || source == target || (target && isTreeItemAncestor(source, target))) {
            m_dragSources.clear();
            event->ignore();
            return;
        }
    }

    QTreeWidgetItem *destinationParent = nullptr;
    int destinationIndex = topLevelItemCount();

    if (target) {
        const DropIndicatorPosition indicator = dropIndicatorPosition();
        const LayerType targetType = static_cast<LayerType>(
            target->data(0, LayerTypeRole).toInt());

        if (indicator == QAbstractItemView::OnItem && targetType == LayerType::Group) {
            destinationParent = target;
            destinationIndex = 0;
        } else {
            destinationParent = target->parent();
            destinationIndex = itemIndex(this, target);
            if (indicator == QAbstractItemView::BelowItem) {
                ++destinationIndex;
            }
        }
    }

    QVector<QUuid> ids;
    ids.reserve(sources.size());
    for (QTreeWidgetItem *source : sources) {
        const QUuid id(source->data(0, LayerIdRole).toString());
        if (!id.isNull()) {
            ids.push_back(id);
        }
    }
    const QUuid destinationParentId = destinationParent
        ? QUuid(destinationParent->data(0, LayerIdRole).toString())
        : QUuid();

    // Do not let QTreeWidget perform its own delayed internal move. The model
    // is updated first, then MainWindow rebuilds the visible tree immediately.
    event->setDropAction(Qt::MoveAction);
    event->accept();
    m_dragSources.clear();

    // Rebuild after QAbstractItemView has completely left its drag handler.
    // Clearing/repopulating the tree re-entrantly from dropEvent can leave Qt's
    // internal persistent indexes pointing at the old children until another
    // insertion happens — the exact delayed-refresh failure this model-driven
    // path is intended to remove.
    QTimer::singleShot(0, this, [this, ids, destinationParentId, destinationIndex] {
        emit layersDropRequested(ids, destinationParentId, destinationIndex);
    });
}

bool LayerTreeWidget::moveSelectionUp()
{
    const QList<QTreeWidgetItem *> roots = selectedRootItems();
    bool changed = false;
    setUpdatesEnabled(false);
    for (QTreeWidgetItem *item : roots) {
        QTreeWidgetItem *parent = item->parent();
        const int index = itemIndex(this, item);
        if (index <= 0) {
            continue;
        }
        QTreeWidgetItem *previous = parent ? parent->child(index - 1)
                                           : topLevelItem(index - 1);
        if (previous && previous->isSelected()) {
            continue;
        }
        QTreeWidgetItem *moved = takeItem(this, item);
        insertItem(this, parent, index - 1, moved);
        changed = true;
    }
    setUpdatesEnabled(true);
    if (changed) {
        doItemsLayout();
        viewport()->update();
        emit structureChanged();
    }
    return changed;
}

bool LayerTreeWidget::moveSelectionDown()
{
    QList<QTreeWidgetItem *> roots = selectedRootItems();
    std::reverse(roots.begin(), roots.end());
    bool changed = false;
    setUpdatesEnabled(false);
    for (QTreeWidgetItem *item : roots) {
        QTreeWidgetItem *parent = item->parent();
        const int index = itemIndex(this, item);
        const int count = parent ? parent->childCount() : topLevelItemCount();
        if (index < 0 || index >= count - 1) {
            continue;
        }
        QTreeWidgetItem *next = parent ? parent->child(index + 1)
                                       : topLevelItem(index + 1);
        if (next && next->isSelected()) {
            continue;
        }
        QTreeWidgetItem *moved = takeItem(this, item);
        insertItem(this, parent, index + 1, moved);
        changed = true;
    }
    setUpdatesEnabled(true);
    if (changed) {
        doItemsLayout();
        viewport()->update();
        emit structureChanged();
    }
    return changed;
}

void LayerTreeWidget::drawBranches(QPainter *painter,
                                   const QRect &rect,
                                   const QModelIndex &index) const
{
    if (!painter || !index.isValid()) {
        return;
    }

    const QTreeWidgetItem *item = itemFromIndex(index);
    if (!item || (item->childCount() == 0
                  && item->childIndicatorPolicy() != QTreeWidgetItem::ShowIndicator)) {
        return;
    }

    const double size = 7.0;
    const QPointF centre(rect.right() - indentation() * 0.5, rect.center().y());
    QPainterPath chevron;
    if (item->isExpanded()) {
        chevron.moveTo(centre.x() - size * 0.5, centre.y() - size * 0.25);
        chevron.lineTo(centre.x(), centre.y() + size * 0.35);
        chevron.lineTo(centre.x() + size * 0.5, centre.y() - size * 0.25);
    } else {
        chevron.moveTo(centre.x() - size * 0.25, centre.y() - size * 0.5);
        chevron.lineTo(centre.x() + size * 0.35, centre.y());
        chevron.lineTo(centre.x() - size * 0.25, centre.y() + size * 0.5);
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(palette().color(QPalette::Text), 1.5,
                         Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->drawPath(chevron);
    painter->restore();
}

} // namespace vfx
