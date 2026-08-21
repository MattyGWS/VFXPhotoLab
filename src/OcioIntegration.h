#pragma once

#include "ColourManagement.h"

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

namespace vfx {

struct OcioColourSpaceInfo {
    QString name;
    QString family;
    QString description;
    QString encoding;
    QStringList categories;
    bool sceneReferred = true;
    bool data = false;
};

struct OcioDisplayInfo {
    QString name;
    QStringList views;
    QString defaultView;
};

struct OcioConfigInspection {
    OcioConfigReference reference;
    QVector<OcioColourSpaceInfo> colourSpaces;
    QVector<OcioDisplayInfo> displays;
    QStringList looks;
    QString defaultDisplay;
    QString defaultView;
    QString defaultWorkingSpace;
    bool available = false;
    bool fingerprintMatchesSavedReference = true;
    QString warning;
};

struct OcioBuiltInConfigChoice {
    QString label;
    QString uri;
    QString description;
};

class OcioCpuTransform final {
public:
    ~OcioCpuTransform();
    OcioCpuTransform(const OcioCpuTransform &) = delete;
    OcioCpuTransform &operator=(const OcioCpuTransform &) = delete;

    bool isValid() const;
    QByteArray cacheId() const;

private:
    struct Impl;
    explicit OcioCpuTransform(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;

    friend std::shared_ptr<const OcioCpuTransform> createOcioCpuTransform(
        const OcioConfigReference &,
        const ColourSpaceDescriptor &,
        const ColourSpaceDescriptor &,
        QString *,
        bool);
    friend bool applyOcioCpuTransform(QImage *, const OcioCpuTransform &,
                                     const std::atomic_bool *, QString *);
    friend QColor mapOcioSemanticColour(const QColor &, const OcioCpuTransform &,
                                        bool *);
};

class OcioDisplayTransform final {
public:
    ~OcioDisplayTransform();
    OcioDisplayTransform(const OcioDisplayTransform &) = delete;
    OcioDisplayTransform &operator=(const OcioDisplayTransform &) = delete;

    bool isValid() const;
    QByteArray cacheId() const;

private:
    struct Impl;
    explicit OcioDisplayTransform(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;

    friend std::shared_ptr<const OcioDisplayTransform> createOcioDisplayTransform(
        const OcioConfigReference &, const ColourSpaceDescriptor &,
        const QString &, const QString &, const QString &, QString *);
    friend bool applyOcioDisplayTransform(QImage *, const OcioDisplayTransform &,
                                          const std::atomic_bool *, QString *);
};

bool ocioSupportCompiled();
QString ocioLibraryVersion();
QVector<OcioBuiltInConfigChoice> ocioBuiltInConfigChoices();

bool inspectOcioConfiguration(OcioConfigSource source,
                              const QString &locator,
                              OcioConfigInspection *inspection,
                              QString *errorMessage = nullptr);

bool resolveOcioConfiguration(const OcioConfigReference &reference,
                              OcioConfigInspection *inspection,
                              QString *errorMessage = nullptr);

QVector<ColourSpaceDescriptor> ocioWorkingColourSpaces(
    const OcioConfigInspection &inspection);

// Export may target any non-data colour space in the fingerprint-matched
// configuration. Unlike document working spaces, an export destination does
// not require a Qt matrix/TRC proxy because the encoded file can be explicitly
// untagged when no ICC payload exists.
QVector<ColourSpaceDescriptor> ocioExportColourSpaces(
    const OcioConfigInspection &inspection);

// Returns a matrix/TRC QColorSpace proxy for OCIO working spaces whose
// primaries and transfer function can be represented exactly by Qt. The proxy
// keeps the existing domain-aware CPU adjustment path correct while the
// authoritative identity remains in DocumentColourState.
QColorSpace ocioQtWorkingSpaceProxy(const ColourSpaceDescriptor &descriptor);

std::shared_ptr<const OcioCpuTransform> createOcioCpuTransform(
    const OcioConfigReference &config,
    const ColourSpaceDescriptor &source,
    const ColourSpaceDescriptor &destination,
    QString *errorMessage = nullptr,
    bool requireIntegerWorkingProxy = true);

bool applyOcioCpuTransform(QImage *image,
                           const OcioCpuTransform &transform,
                           const std::atomic_bool *cancelRequested = nullptr,
                           QString *errorMessage = nullptr);

QColor mapOcioSemanticColour(const QColor &colour,
                             const OcioCpuTransform &transform,
                             bool *ok = nullptr);

std::shared_ptr<const OcioDisplayTransform> createOcioDisplayTransform(
    const OcioConfigReference &config,
    const ColourSpaceDescriptor &source,
    const QString &display,
    const QString &view,
    const QString &look = {},
    QString *errorMessage = nullptr);

bool applyOcioDisplayTransform(
    QImage *image,
    const OcioDisplayTransform &transform,
    const std::atomic_bool *cancelRequested = nullptr,
    QString *errorMessage = nullptr);

bool validateOcioDisplayView(const OcioConfigReference &config,
                             const QString &sourceSpace,
                             const QString &display,
                             const QString &view,
                             const QString &look,
                             QString *errorMessage = nullptr);

} // namespace vfx
