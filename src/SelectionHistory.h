#pragma once

#include "SelectionMask.h"

#include <QByteArray>
#include <QPoint>
#include <QSize>
#include <QVector>
#include <QtGlobal>

namespace vfx {

struct SelectionTileDelta {
    QPoint tileIndex;
    QSize tileSize;
    QByteArray payload;
    int rawByteCount = 0;
    quint64 beforeHash = 0;
    quint64 afterHash = 0;
    bool compressed = false;
};

// Symmetric sparse XOR history for the document selection. Metadata changes
// such as Select All <-> active empty remain constant-size because untouched
// implicit tiles are represented only by before/after default coverage.
struct SelectionTileDeltaSet {
    QSize selectionSize;
    bool beforeActive = false;
    bool afterActive = false;
    quint8 beforeImplicitCoverage = 0;
    quint8 afterImplicitCoverage = 0;
    QVector<SelectionTileDelta> tiles;

    bool isEmpty() const;
    qint64 storedBytes() const;
};

SelectionTileDeltaSet buildSelectionTileDeltaSet(
    const SelectionMask::Snapshot &before,
    const SelectionMask::Snapshot &after,
    const QRect &affectedRect = {});

bool applySelectionTileDeltaSet(SelectionMask *selection,
                                const SelectionTileDeltaSet &deltaSet,
                                bool targetAfter);

} // namespace vfx
