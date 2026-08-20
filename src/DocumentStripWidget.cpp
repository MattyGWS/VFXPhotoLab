#include "DocumentStripWidget.h"
#include "AppStyle.h"

#include <QAbstractListModel>
#include <QAction>
#include <QColor>
#include <QContextMenuEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QSizePolicy>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QVariant>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace vfx {
namespace {

constexpr int CardWidth = 126;
constexpr int CardHeight = 88;
constexpr int ThumbnailWidth = 92;
constexpr int ThumbnailHeight = 52;
constexpr int CloseButtonSize = 18;

constexpr int SessionIdRole = Qt::UserRole + 1;
constexpr int ThumbnailRole = Qt::UserRole + 2;
constexpr int ModifiedRole = Qt::UserRole + 3;
constexpr int ResidencyRole = Qt::UserRole + 4;
constexpr int PixelSizeRole = Qt::UserRole + 5;

QString residencyLabel(const SessionResidency residency)
{
    switch (residency) {
    case SessionResidency::Hot: return QObject::tr("Active in memory");
    case SessionResidency::Warm: return QObject::tr("Ready in memory");
    case SessionResidency::Cold: return QObject::tr("Stored safely on disk");
    }
    return {};
}

QRect thumbnailRect(const QRect &card)
{
    return QRect(card.left() + (card.width() - ThumbnailWidth) / 2,
                 card.top() + 4,
                 ThumbnailWidth,
                 ThumbnailHeight);
}

QRect closeRect(const QRect &card)
{
    return QRect(card.right() - CloseButtonSize - 4,
                 card.bottom() - CloseButtonSize - 3,
                 CloseButtonSize,
                 CloseButtonSize);
}

void paintChecker(QPainter *painter, const QRect &rect)
{
    constexpr int checkerSize = 6;
    const QColor light = themeColour(QStringLiteral("checker_light"));
    const QColor dark = themeColour(QStringLiteral("checker_dark"));
    painter->save();
    painter->setClipRect(rect);
    for (int y = rect.top(); y <= rect.bottom(); y += checkerSize) {
        for (int x = rect.left(); x <= rect.right(); x += checkerSize) {
            painter->fillRect(QRect(x, y, checkerSize, checkerSize),
                              (((x - rect.left()) / checkerSize
                                + (y - rect.top()) / checkerSize) & 1)
                                  ? light
                                  : dark);
        }
    }
    painter->restore();
}

class DocumentDelegate final : public QStyledItemDelegate {
public:
    explicit DocumentDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(CardWidth, CardHeight);
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!painter || !index.isValid()) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect card = option.rect.adjusted(1, 1, -1, -1);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const QColor background = selected
            ? themeColour(QStringLiteral("selection"))
            : hovered ? themeColour(QStringLiteral("button_hover"))
                      : themeColour(QStringLiteral("panel"));
        const QColor border = selected
            ? themeColour(QStringLiteral("accent"))
            : hovered ? themeColour(QStringLiteral("border_strong"))
                      : themeColour(QStringLiteral("border"));
        painter->setPen(QPen(border, selected ? 2.0 : 1.0));
        painter->setBrush(background);
        painter->drawRoundedRect(QRectF(card), 5.0, 5.0);

        const QRect thumb = thumbnailRect(card);
        paintChecker(painter, thumb);
        const QImage image = qvariant_cast<QImage>(index.data(ThumbnailRole));
        if (!image.isNull()) {
            const QImage scaled = image.scaled(thumb.size(),
                                               Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
            painter->drawImage(QPoint(thumb.center().x() - scaled.width() / 2,
                                      thumb.center().y() - scaled.height() / 2),
                               scaled);
        }
        painter->setPen(QPen(themeColour(QStringLiteral("border_strong")), 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(thumb.adjusted(0, 0, -1, -1));

        const bool modified = index.data(ModifiedRole).toBool();
        const QRect close = closeRect(card);
        const int captionLeft = card.left() + 6;
        const int dirtyWidth = modified ? 11 : 0;
        const QRect titleRect(captionLeft + dirtyWidth,
                              card.bottom() - 23,
                              std::max(8, close.left() - captionLeft - dirtyWidth - 3),
                              19);
        if (modified) {
            painter->setPen(themeColour(QStringLiteral("accent_hover")));
            painter->drawText(QRect(captionLeft, titleRect.top(), 10, titleRect.height()),
                              Qt::AlignCenter,
                              QStringLiteral("●"));
        }

        QFont titleFont = option.font;
        titleFont.setPointSizeF(std::max(7.0, titleFont.pointSizeF() - 1.0));
        painter->setFont(titleFont);
        painter->setPen(themeColour(QStringLiteral("text")));
        const QString title = index.data(Qt::DisplayRole).toString();
        const QString elided = QFontMetrics(titleFont).elidedText(title,
                                                                 Qt::ElideMiddle,
                                                                 titleRect.width());
        painter->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, elided);

        painter->setPen(themeColour(QStringLiteral("text_muted")));
        QFont closeFont = option.font;
        closeFont.setBold(true);
        closeFont.setPointSizeF(std::max(10.0, closeFont.pointSizeF() + 1.0));
        painter->setFont(closeFont);
        painter->drawText(close, Qt::AlignCenter, QStringLiteral("×"));
        painter->restore();
    }
};

} // namespace

class DocumentStripWidget::Model final : public QAbstractListModel {
public:
    struct Entry {
        QUuid sessionId;
        QString displayName;
        QImage thumbnail;
        bool modified = false;
        SessionResidency residency = SessionResidency::Warm;
        QSize pixelSize;
    };

    explicit Model(QObject *parent = nullptr)
        : QAbstractListModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
    }

    QVariant data(const QModelIndex &index, const int role) const override
    {
        if (!index.isValid() || index.row() < 0
            || index.row() >= static_cast<int>(m_entries.size())) {
            return {};
        }
        const Entry &entry = m_entries.at(static_cast<std::size_t>(index.row()));
        switch (role) {
        case Qt::DisplayRole: return entry.displayName;
        case Qt::ToolTipRole: {
            QStringList details;
            details.push_back(entry.displayName);
            if (entry.pixelSize.isValid() && !entry.pixelSize.isEmpty()) {
                details.push_back(tr("%1 × %2 px")
                                      .arg(entry.pixelSize.width())
                                      .arg(entry.pixelSize.height()));
            }
            details.push_back(residencyLabel(entry.residency));
            if (entry.modified) {
                details.push_back(tr("Unsaved changes"));
            }
            return details.join(QStringLiteral("\n"));
        }
        case SessionIdRole: return entry.sessionId;
        case ThumbnailRole: return entry.thumbnail;
        case ModifiedRole: return entry.modified;
        case ResidencyRole: return static_cast<int>(entry.residency);
        case PixelSizeRole: return entry.pixelSize;
        default: return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        return index.isValid()
            ? Qt::ItemIsEnabled | Qt::ItemIsSelectable
            : Qt::NoItemFlags;
    }

    int rowForId(const QUuid &sessionId) const
    {
        for (int row = 0; row < static_cast<int>(m_entries.size()); ++row) {
            if (m_entries.at(static_cast<std::size_t>(row)).sessionId == sessionId) {
                return row;
            }
        }
        return -1;
    }

    QUuid idAt(const int row) const
    {
        return row >= 0 && row < static_cast<int>(m_entries.size())
            ? m_entries.at(static_cast<std::size_t>(row)).sessionId
            : QUuid();
    }

    void upsert(Entry entry)
    {
        const int row = rowForId(entry.sessionId);
        if (row < 0) {
            const int insertedRow = static_cast<int>(m_entries.size());
            beginInsertRows({}, insertedRow, insertedRow);
            m_entries.push_back(std::move(entry));
            endInsertRows();
            return;
        }
        Entry &current = m_entries[static_cast<std::size_t>(row)];
        const bool thumbnailMatches = current.thumbnail.isNull() == entry.thumbnail.isNull()
            && (current.thumbnail.isNull()
                || current.thumbnail.cacheKey() == entry.thumbnail.cacheKey());
        if (current.displayName == entry.displayName
            && thumbnailMatches
            && current.modified == entry.modified
            && current.residency == entry.residency
            && current.pixelSize == entry.pixelSize) {
            return;
        }
        current = std::move(entry);
        emit dataChanged(index(row, 0), index(row, 0));
    }

    void remove(const QUuid &sessionId)
    {
        const int row = rowForId(sessionId);
        if (row < 0) {
            return;
        }
        beginRemoveRows({}, row, row);
        m_entries.erase(m_entries.begin() + row);
        endRemoveRows();
    }

    void clear()
    {
        if (m_entries.empty()) {
            return;
        }
        beginResetModel();
        m_entries.clear();
        endResetModel();
    }

private:
    std::vector<Entry> m_entries;
};

class DocumentStripWidget::View final : public QListView {
public:
    explicit View(QWidget *parent = nullptr)
        : QListView(parent)
    {
        setObjectName(QStringLiteral("DocumentStripView"));
        setFlow(QListView::LeftToRight);
        setWrapping(false);
        setResizeMode(QListView::Adjust);
        setLayoutMode(QListView::Batched);
        setBatchSize(64);
        setMovement(QListView::Static);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setUniformItemSizes(true);
        setSpacing(5);
        setMouseTracking(true);
        setFrameShape(QFrame::NoFrame);
        setContextMenuPolicy(Qt::DefaultContextMenu);
    }

    std::function<void(const QModelIndex &)> activateRequested;
    std::function<void(const QModelIndex &)> closeRequested;
    std::function<void(const QModelIndex &, const QPoint &)> contextRequested;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (event->button() == Qt::LeftButton && index.isValid()) {
            const QRect card = visualRect(index).adjusted(1, 1, -1, -1);
            if (closeRect(card).contains(event->position().toPoint())) {
                if (closeRequested) {
                    closeRequested(index);
                }
                event->accept();
                return;
            }
            // Let the view finish its normal mouse selection first. Activation
            // may synchronously restore a Cold document or reject the switch;
            // doing it afterwards ensures a rejected switch can reselect the
            // actual active card without the base handler overriding it.
            QListView::mouseReleaseEvent(event);
            setCurrentIndex(index);
            if (activateRequested) {
                activateRequested(index);
            }
            return;
        }
        QListView::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
             || event->key() == Qt::Key_Space)
            && currentIndex().isValid()) {
            if (activateRequested) {
                activateRequested(currentIndex());
            }
            event->accept();
            return;
        }
        QListView::keyPressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const QModelIndex index = indexAt(event->pos());
        if (index.isValid() && contextRequested) {
            contextRequested(index, event->globalPos());
            event->accept();
            return;
        }
        QListView::contextMenuEvent(event);
    }
};

DocumentStripWidget::DocumentStripWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("DocumentStrip"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(111);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(5, 4, 5, 3);
    layout->setSpacing(0);

    m_model = new Model(this);
    m_view = new View(this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(new DocumentDelegate(m_view));
    layout->addWidget(m_view);

    m_view->activateRequested = [this](const QModelIndex &index) {
        const QUuid id = index.data(SessionIdRole).toUuid();
        if (!id.isNull()) {
            emit documentActivated(id);
        }
    };
    m_view->closeRequested = [this](const QModelIndex &index) {
        const QUuid id = index.data(SessionIdRole).toUuid();
        if (!id.isNull()) {
            emit closeDocumentRequested(id);
        }
    };
    m_view->contextRequested = [this](const QModelIndex &index, const QPoint &globalPosition) {
        const QUuid id = index.data(SessionIdRole).toUuid();
        if (id.isNull()) {
            return;
        }
        QMenu menu(this);
        QAction *close = menu.addAction(tr("Close"));
        QAction *closeOthers = menu.addAction(tr("Close Others"));
        QAction *closeAll = menu.addAction(tr("Close All"));
        QAction *chosen = menu.exec(globalPosition);
        if (chosen == close) {
            emit closeDocumentRequested(id);
        } else if (chosen == closeOthers) {
            emit closeOtherDocumentsRequested(id);
        } else if (chosen == closeAll) {
            emit closeAllDocumentsRequested();
        }
    };

    updateStripVisibility();
}

void DocumentStripWidget::upsertDocument(const QUuid &sessionId,
                                         const QString &displayName,
                                         const QImage &thumbnail,
                                         const bool modified,
                                         const SessionResidency residency,
                                         const QSize &pixelSize)
{
    if (sessionId.isNull()) {
        return;
    }
    Model::Entry entry;
    entry.sessionId = sessionId;
    entry.displayName = displayName.trimmed().isEmpty() ? tr("Untitled")
                                                        : displayName.trimmed();
    entry.thumbnail = thumbnail;
    entry.modified = modified;
    entry.residency = residency;
    entry.pixelSize = pixelSize;
    m_model->upsert(std::move(entry));
    updateStripVisibility();
    if (sessionId == m_activeDocumentId) {
        setActiveDocument(sessionId);
    }
}

void DocumentStripWidget::removeDocument(const QUuid &sessionId)
{
    m_model->remove(sessionId);
    if (m_activeDocumentId == sessionId) {
        m_activeDocumentId = {};
    }
    updateStripVisibility();
}

void DocumentStripWidget::clearDocuments()
{
    m_model->clear();
    m_activeDocumentId = {};
    updateStripVisibility();
}

void DocumentStripWidget::setActiveDocument(const QUuid &sessionId)
{
    m_activeDocumentId = sessionId;
    const int row = m_model->rowForId(sessionId);
    if (row < 0) {
        m_view->clearSelection();
        m_view->setCurrentIndex(QModelIndex());
        return;
    }
    const QModelIndex index = m_model->index(row, 0);
    m_view->setCurrentIndex(index);
    m_view->selectionModel()->select(index,
                                     QItemSelectionModel::ClearAndSelect
                                         | QItemSelectionModel::Rows);
    m_view->scrollTo(index, QAbstractItemView::EnsureVisible);
}

QUuid DocumentStripWidget::activeDocument() const
{
    return m_activeDocumentId;
}

int DocumentStripWidget::documentCount() const
{
    return m_model->rowCount();
}

void DocumentStripWidget::updateStripVisibility()
{
    setVisible(m_model->rowCount() > 0);
}

} // namespace vfx
