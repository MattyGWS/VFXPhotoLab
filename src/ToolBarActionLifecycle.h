#pragma once

#include <QAction>
#include <QList>
#include <QString>
#include <QToolBar>
#include <QVariant>

namespace vfx {

enum class ToolBarActionDisposal {
    Deferred,
    Immediate
};

inline constexpr const char *DynamicToolBarActionProperty =
    "vfxDynamicToolBarAction";

// QToolBar::clear() removes actions from the visible toolbar but leaves their
// QObject lifetime with the original owner. Dynamically rebuilt toolbars must
// retire actions that the toolbar itself owns, while preserving shared actions
// owned by menus, the main window, or another object. Rebuilds defer deletion
// because they are often triggered from a control's own signal; shutdown may
// delete immediately after event delivery has stopped.
inline int disposeToolBarOwnedActions(
    QToolBar *toolBar,
    const ToolBarActionDisposal disposal = ToolBarActionDisposal::Deferred)
{
    if (!toolBar) {
        return 0;
    }

    int disposed = 0;
    const QList<QAction *> actions = toolBar->actions();
    for (QAction *action : actions) {
        if (!action) {
            continue;
        }
        const bool ownedByToolBar = action->parent() == toolBar;
        toolBar->removeAction(action);
        if (!ownedByToolBar) {
            continue;
        }

        action->setProperty(DynamicToolBarActionProperty, true);
        if (disposal == ToolBarActionDisposal::Deferred) {
            action->deleteLater();
        } else {
            delete action;
        }
        ++disposed;
    }
    return disposed;
}

// If the application closes before deferred deletions are delivered, destroy
// only the dynamic toolbar-owned actions marked by disposeToolBarOwnedActions.
// Internal QToolBar actions and externally owned shared actions are untouched.
inline int destroyDeferredToolBarOwnedActions(QToolBar *toolBar)
{
    if (!toolBar) {
        return 0;
    }

    int destroyed = 0;
    const QList<QAction *> children = toolBar->findChildren<QAction *>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QAction *action : children) {
        if (!action
            || !action->property(DynamicToolBarActionProperty).toBool()) {
            continue;
        }
        delete action;
        ++destroyed;
    }
    return destroyed;
}

} // namespace vfx
