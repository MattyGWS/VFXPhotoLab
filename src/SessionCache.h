#pragma once

#include "DocumentSession.h"

#include <QHash>
#include <QString>
#include <QUuid>

#include <functional>
#include <memory>

class QLockFile;

namespace vfx {

// Private application-cache storage for inactive documents. This is deliberately
// separate from the public .vfxphoto project format: payloads are raw, exact and
// independently compressed rather than PNG/base64 encoded JSON.
class SessionCacheStore final {
public:
    explicit SessionCacheStore(QString rootPath = {});
    ~SessionCacheStore();

    SessionCacheStore(const SessionCacheStore &) = delete;
    SessionCacheStore &operator=(const SessionCacheStore &) = delete;

    bool isAvailable() const;
    const QString &rootPath() const;
    const QString &runPath() const;
    QString snapshotPath(const QUuid &sessionId) const;

    bool writeSnapshot(const DocumentSession &session,
                       QString *writtenPath = nullptr,
                       qint64 *writtenBytes = nullptr,
                       QString *errorMessage = nullptr);
    bool restoreSnapshot(const QString &filePath,
                         DocumentSession *session,
                         QString *errorMessage = nullptr) const;
    bool removeSnapshot(const QString &filePath,
                        QString *errorMessage = nullptr) const;

    // Removes unlocked abandoned per-run directories left by crashes. The
    // current run and directories owned by other live processes are untouched.
    int cleanupStaleRuns(int maximumAgeDays = 7) const;

private:
    QString m_rootPath;
    QString m_runPath;
    bool m_available = false;
    std::unique_ptr<QLockFile> m_runLock;
};

class DocumentResidencyManager final {
public:
    struct Limits {
        qint64 residentDocumentBytes = 1536LL * 1024LL * 1024LL;
        int warmSessionCount = 24;
    };

    struct Stats {
        int registeredSessions = 0;
        int hotSessions = 0;
        int warmSessions = 0;
        int coldSessions = 0;
        int historiesDiscardedForColdStorage = 0;
        qint64 residentDocumentBytes = 0;
        qint64 historyBytes = 0;
        qint64 backingBytes = 0;
    };

    DocumentResidencyManager();
    explicit DocumentResidencyManager(Limits limits, QString cacheRoot = {});
    ~DocumentResidencyManager();

    DocumentResidencyManager(const DocumentResidencyManager &) = delete;
    DocumentResidencyManager &operator=(const DocumentResidencyManager &) = delete;

    void registerSession(DocumentSession *session);
    void unregisterSession(DocumentSession *session, bool removeBackingSnapshot = true);

    // Restores a cold session before activation, marks the previous active
    // session warm and enforces the process-wide resident-document budget.
    bool activateSession(DocumentSession *session, QString *errorMessage = nullptr);
    void touchSession(DocumentSession *session);
    bool enforceLimits(QString *errorMessage = nullptr);

    void discardBackingSnapshot(DocumentSession *session);
    void setLimits(const Limits &limits);
    const Limits &limits() const;
    Stats stats() const;

    const SessionCacheStore &cacheStore() const;
    SessionCacheStore &cacheStore();

    // The GUI supplies this so a successfully Cold session can release the
    // matching CPU/GPU tile namespace without coupling this core class to
    // RenderBackend. It is not invoked when the atomic snapshot fails.
    void setColdEvictionCallback(std::function<void(const QUuid &)> callback);

private:
    struct Entry {
        quint64 accessOrder = 0;
    };

    DocumentSession *oldestWarmSession() const;
    DocumentSession *oldestColdSessionWithHistory() const;
    int warmSessionCount() const;
    qint64 residentPixelBytes() const;
    qint64 residentBytes() const;

    Limits m_limits;
    SessionCacheStore m_cacheStore;
    QHash<DocumentSession *, Entry> m_entries;
    DocumentSession *m_activeSession = nullptr;
    quint64 m_accessCounter = 0;
    std::function<void(const QUuid &)> m_onColdEviction;
};

} // namespace vfx
