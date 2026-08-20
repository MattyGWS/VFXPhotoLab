#pragma once

#include "Adjustment.h"
#include "ColourManagement.h"
#include "SelectionMask.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QUuid>
#include <QVector>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace vfx {

enum class HistogramChannel {
    Rgb,
    Red,
    Green,
    Blue,
    Alpha
};

enum class HistogramScope {
    Document,
    Selection
};

struct HistogramData {
    int binCount = 0;
    int sourceBitDepth = 8;
    bool exact = true;
    bool cancelled = false;
    bool adjustmentFound = false;
    quint64 includedWeight = 0;
    quint64 includedPixels = 0;
    quint64 transparentPixels = 0;
    quint64 selectionRevision = 0;
    QVector<quint64> luminance;
    QVector<quint64> red;
    QVector<quint64> green;
    QVector<quint64> blue;
    QVector<quint64> alpha;

    const QVector<quint64> &channel(HistogramChannel channel) const;
    qint64 estimatedBytes() const;
    bool isValid() const;
};

struct HistogramRequest {
    QUuid documentSessionId;
    QUuid adjustmentLayerId;
    QUuid liveFilterOwnerId;
    QUuid liveFilterId;
    quint64 documentRevision = 0;
    quint64 colourStateRevision = 0;
    ColourProcessingCompatibility processingCompatibility =
        ColourProcessingCompatibility::LegacyV1;
    QImage source;
    QVector<LayerNode> layers;
    QSize documentSize;
    HistogramScope scope = HistogramScope::Document;
    SelectionMask::Snapshot selection;

    QString cacheKey() const;
};

class HistogramService final : public QObject {
    Q_OBJECT

public:
    using Completion = std::function<void(quint64, const HistogramData &)>;

    explicit HistogramService(QObject *parent = nullptr);
    ~HistogramService() override;

    quint64 request(const HistogramRequest &request, Completion completion);
    void cancel();
    quint64 activeRequestSerial() const;

    static HistogramData calculate(const HistogramRequest &request,
                                   const std::atomic_bool *cancelRequested = nullptr);

private:
    quint64 m_requestSerial = 0;
    std::shared_ptr<std::atomic_bool> m_cancelRequested;
};

} // namespace vfx
