#include "ShortcutUtils.h"

namespace vfx {

QList<QKeySequence> redoKeySequences()
{
    QList<QKeySequence> sequences;
    const auto appendUnique = [&sequences](const QKeySequence &sequence) {
        if (!sequence.isEmpty() && !sequences.contains(sequence)) {
            sequences.append(sequence);
        }
    };

    for (const QKeySequence &sequence : QKeySequence::keyBindings(QKeySequence::Redo)) {
        appendUnique(sequence);
    }
    appendUnique(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
    appendUnique(QKeySequence(Qt::CTRL | Qt::Key_Y));
    return sequences;
}

} // namespace vfx
