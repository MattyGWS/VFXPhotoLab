#pragma once

#include "ColourManagement.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>
#include <QtCore/qfloat16.h>

#include <atomic>
#include <memory>

class QScreen;

namespace vfx {

enum class MonitorProfileSource : quint8 {
    ManualOverride,
    Colord,
    WindowsWcs,
    Environment,
    SRgbFallback
};

struct MonitorProfileInfo {
    ColourSpaceDescriptor profile;
    QString screenName;
    QString profilePath;
    QString sourceLabel;
    QString status;
    MonitorProfileSource source = MonitorProfileSource::SRgbFallback;
    bool detected = false;
    bool reliable = false;
    bool fallback = true;

    QByteArray stableFingerprint() const;
};

MonitorProfileInfo discoverMonitorProfile(const QScreen *screen,
                                          const QString &manualOverridePath = {});


struct DisplayGpuLutData {
    int edgeSize = 0;
    QVector<qfloat16> forwardRgba16f;
    QVector<qfloat16> gamutRoundTripRgba16f;
    QByteArray fingerprint;
    float gamutWarningThreshold = 14.0f;
    int referenceMaximumDifference = -1;
    bool gamutWarning = false;

    qsizetype texelCount() const
    {
        return edgeSize > 1
            ? qsizetype(edgeSize) * edgeSize * edgeSize
            : 0;
    }

    bool isValid() const
    {
        return edgeSize > 1
            && forwardRgba16f.size() == texelCount() * 4
            && (!gamutWarning
                || gamutRoundTripRgba16f.size() == texelCount() * 4);
    }
};

struct DisplayColourTransformStatus {
    bool active = false;
    bool fallback = false;
    bool proofingActive = false;
    bool gamutWarningActive = false;
    QString summary;
    QString warning;
    QByteArray fingerprint;
};

class DisplayColourTransform final {
public:
    ~DisplayColourTransform();
    DisplayColourTransform(const DisplayColourTransform &) = delete;
    DisplayColourTransform &operator=(const DisplayColourTransform &) = delete;

    bool isIdentity() const;
    QByteArray fingerprint() const;
    DisplayColourTransformStatus status() const;
    bool apply(QImage *image,
               const std::atomic_bool *cancelRequested = nullptr,
               QString *errorMessage = nullptr) const;
    std::shared_ptr<const DisplayGpuLutData> gpuLutData(
        QString *errorMessage = nullptr) const;

private:
    struct Impl;
    explicit DisplayColourTransform(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;

    friend std::shared_ptr<const DisplayColourTransform>
    createDisplayColourTransform(const DocumentColourState &,
                                 const MonitorProfileInfo &,
                                 QString *);
};

std::shared_ptr<const DisplayColourTransform> createDisplayColourTransform(
    const DocumentColourState &state,
    const MonitorProfileInfo &monitor,
    QString *errorMessage = nullptr);

QString colourRenderingIntentName(ColourRenderingIntent intent);

} // namespace vfx
