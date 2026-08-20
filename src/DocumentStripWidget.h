#pragma once

#include "DocumentSession.h"

#include <QImage>
#include <QUuid>
#include <QWidget>

namespace vfx {

// Compact, horizontally scrolling switcher for open document sessions. It is
// model/delegate based, so hundreds of open documents do not create hundreds
// of nested thumbnail widgets. Editable pixels remain in DocumentSession and
// are governed by DocumentResidencyManager.
class DocumentStripWidget final : public QWidget {
    Q_OBJECT

public:
    explicit DocumentStripWidget(QWidget *parent = nullptr);

    void upsertDocument(const QUuid &sessionId,
                        const QString &displayName,
                        const QImage &thumbnail,
                        bool modified,
                        SessionResidency residency,
                        const QSize &pixelSize = {});
    void removeDocument(const QUuid &sessionId);
    void clearDocuments();
    void setActiveDocument(const QUuid &sessionId);
    QUuid activeDocument() const;
    int documentCount() const;

signals:
    void documentActivated(const QUuid &sessionId);
    void closeDocumentRequested(const QUuid &sessionId);
    void closeOtherDocumentsRequested(const QUuid &sessionId);
    void closeAllDocumentsRequested();

private:
    class Model;
    class View;

    void updateStripVisibility();

    Model *m_model = nullptr;
    View *m_view = nullptr;
    QUuid m_activeDocumentId;
};

} // namespace vfx
