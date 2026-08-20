#pragma once

#include <QKeySequence>
#include <QList>

namespace vfx {

// Return the platform's standard Redo bindings plus the two conventions
// users commonly expect on Linux and Windows. Entries are de-duplicated so
// Qt's shortcut map cannot treat the same sequence as ambiguous.
QList<QKeySequence> redoKeySequences();

} // namespace vfx
