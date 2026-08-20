#pragma once

#include "TransformInterpolation.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>
#include <QtGlobal>

namespace vfx {

// 0.14.0a intentionally separates a Smart Layer instance from the source it
// presents. The source registry is document-owned; instances store only this
// lightweight reference plus their ordinary layer transform/mask/compositing
// state. Embedded source contents are introduced by 0.14.0b without changing
// this identity contract.
enum class SmartSourceStorage : quint8 {
    Embedded = 0,
    Linked = 1
};

QString smartSourceStorageToString(SmartSourceStorage storage);
SmartSourceStorage smartSourceStorageFromString(const QString &value,
                                                bool *ok = nullptr);

struct SmartTransformState {
    static constexpr quint32 CurrentSchema = 1;

    quint32 schema = CurrentSchema;
    // Smart transforms never bake the source presentation. This setting only
    // controls how the current authoritative source is sampled into the
    // containing document after the instance transform has been composed.
    // Bilinear is the migration/default value so 0.14.0a-c projects preserve
    // their established appearance until a new transform explicitly chooses
    // another method.
    TransformInterpolation interpolation = TransformInterpolation::Bilinear;

    bool isSafe() const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static SmartTransformState fromJson(const QJsonObject &object,
                                        bool *ok = nullptr);
    bool operator==(const SmartTransformState &) const = default;
};

struct SmartLayerReference {
    static constexpr quint32 CurrentSchema = 1;

    quint32 schema = CurrentSchema;
    QUuid sourceId;
    // Last source revision observed by this instance. It is not authoritative:
    // the registry descriptor owns the current source revision. Keeping it on
    // the instance gives render/cache code an explicit stale-instance signal.
    quint64 observedSourceRevision = 1;

    bool isEmpty() const { return sourceId.isNull(); }
    bool isSafe() const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static SmartLayerReference fromJson(const QJsonObject &object,
                                        bool *ok = nullptr);
    bool operator==(const SmartLayerReference &) const = default;
};

struct SmartSourceDescriptor {
    // Descriptor schema 3 adds explicit linked-document identity and resolved
    // content fingerprints. Schemas 1-2 remain readable for embedded sources.
    static constexpr quint32 CurrentSchema = 3;
    // Embedded-document schema 2 adds authoritative source bit depth. Schema 3
    // permits nested Smart Layer transform sampling metadata. Schema 4 permits per-instance
    // Live Filter stacks. Schema 5 permits per-Live-Filter masks. Schema 6 permits
    // persistent per-layer Layer Effect stack definitions. Schema 7 permits
    // shadows/glows renderer parameters; schema 8 permits Stroke and Overlay
    // renderer parameters; schema 9 permits Bevel & Emboss authored lighting
    // parameters; schema 10 permits linked Smart Source metadata inside embedded
    // source documents. Earlier schemas remain readable and migrate.
    static constexpr quint32 EmbeddedDocumentSchema = 10;
    static constexpr int MaximumDependencies = 4096;

    quint32 schema = CurrentSchema;
    QUuid id = QUuid::createUuid();
    quint64 revision = 1;
    SmartSourceStorage storage = SmartSourceStorage::Embedded;
    QString name;
    // Linked paths are persisted relative to the owning .vfxphoto when possible
    // and resolved canonically at runtime; an empty path is invalid for Linked.
    QString linkedPath;
    // 0.14.0l persistent identity for the external .vfxphoto document. Path and
    // identity are deliberately separate: Relink may change the path only when
    // this identity still matches, while Replace Source explicitly adopts a new
    // identity. linkedContentFingerprint tracks the resolved authoritative
    // revision: direct project bytes plus nested linked-source fingerprints.
    QUuid linkedDocumentId;
    QByteArray linkedContentFingerprint;
    // Runtime availability is intentionally not persisted. A missing or mismatched
    // link keeps its last safe presentation cache but is never silently rebound.
    bool linkedAvailable = false;
    QString linkedRuntimeWarning;
    // 0.14.0m runtime-only resolved document-identity closure. The target
    // document itself and every currently resolved nested linked document are
    // recorded here after a successful refresh. It is deliberately not
    // persisted: Hot/Warm/Cold restoration re-resolves the graph, and keeping
    // this transient closure lets open-document save propagation skip unrelated
    // linked sources without weakening transitive A -> B -> C updates.
    QVector<QUuid> linkedResolvedDocumentIds;
    // Explicit source-to-source dependencies form the cycle-detection graph.
    // 0.14.0b/c populate this from embedded/nested source contents.
    QVector<QUuid> dependencies;

    // 0.14.0b authoritative embedded contents. The payload deliberately uses
    // the existing LayerNode JSON contract instead of introducing a second
    // semantic layer model. Keeping it opaque here avoids a circular include:
    // LayerNode owns SmartLayerReference, while the document/operations layer
    // validates and decodes this object into a normal editable layer tree.
    QJsonObject embeddedDocument;

    // Exact derived presentation snapshot for the source revision. This is not
    // authoritative and never replaces embeddedDocument. Starting in 0.14.0e,
    // parent rendering addresses this snapshot through bounded 256x256 source
    // tiles and transform-aware intermediate requests; the full snapshot is
    // rebuilt only at an authoritative embedded-source revision boundary.
    QImage presentationImage;
    QRect presentationBounds;
    quint64 presentationRevision = 0;

    bool hasEmbeddedDocument() const { return !embeddedDocument.isEmpty(); }
    bool hasCurrentPresentation() const
    {
        return !presentationImage.isNull()
            && !presentationBounds.isEmpty()
            && presentationRevision == revision;
    }

    bool isSafe(QString *errorMessage = nullptr) const;
    qint64 estimatedBytes() const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static SmartSourceDescriptor fromJson(const QJsonObject &object,
                                          bool *ok = nullptr,
                                          QString *errorMessage = nullptr);
    bool operator==(const SmartSourceDescriptor &other) const;
};

class SmartSourceRegistry final {
public:
    static constexpr int MaximumSources = 8192;

    bool isEmpty() const { return m_sources.isEmpty(); }
    int size() const { return m_sources.size(); }
    qint64 estimatedBytes() const;
    bool contains(const QUuid &id) const { return m_sources.contains(id); }
    QVector<SmartSourceDescriptor> descriptors() const;
    const SmartSourceDescriptor *find(const QUuid &id) const;

    bool insert(const SmartSourceDescriptor &descriptor,
                QString *errorMessage = nullptr);
    bool replace(const SmartSourceDescriptor &descriptor,
                 QString *errorMessage = nullptr);
    // Adopt semantic source contents edited in a child/source document while
    // retaining this registry's authoritative revision number. The caller
    // must subsequently invalidate/bump the source revision. expectedRevision
    // prevents a stale source editor from overwriting a source that changed in
    // its owner while the editor was open.
    bool adoptEditedDescriptor(const SmartSourceDescriptor &descriptor,
                               quint64 expectedRevision,
                               QString *errorMessage = nullptr);
    bool remove(const QUuid &id);
    bool bumpRevision(const QUuid &id, quint64 *newRevision = nullptr);
    // A source edit also changes every source whose embedded contents depend
    // on it. Revision propagation is therefore transitive, but still bounded
    // to the dependency subgraph rather than invalidating the whole document.
    bool bumpRevisionCascade(const QUuid &id,
                             QHash<QUuid, quint64> *changedRevisions = nullptr);

    bool validate(QString *errorMessage = nullptr) const;
    bool wouldIntroduceCycle(const QUuid &sourceId,
                             const QVector<QUuid> &dependencies) const;

    QJsonArray toJson(bool *ok = nullptr) const;
    static SmartSourceRegistry fromJson(const QJsonArray &array,
                                        bool *ok = nullptr,
                                        QStringList *warnings = nullptr);

    bool operator==(const SmartSourceRegistry &) const = default;

private:
    QHash<QUuid, SmartSourceDescriptor> m_sources;
};

} // namespace vfx
