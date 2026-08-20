#include "HistogramService.h"
#include "ImageProcessor.h"

#include <QByteArray>
#include <QCache>
#include <QFutureWatcher>
#include <QMutex>
#include <QMutexLocker>
#include <QPoint>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent>
#include <QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <utility>

namespace vfx {
namespace {

constexpr int HistogramCacheKiB = 32 * 1024;
constexpr qint64 ParallelHistogramPixelThreshold = 1024LL * 1024LL;

QThreadPool *histogramWorkerPool()
{
    static QThreadPool pool;
    static std::once_flag configured;
    std::call_once(configured, [] {
        // Keep this separate from the global pool because HistogramService::request
        // itself runs on that pool. A bounded private pool avoids nested-pool
        // starvation while keeping exact 16-bit per-worker bins memory-safe.
        pool.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 1, 8));
        pool.setExpiryTimeout(30'000);
    });
    return &pool;
}

struct HistogramAccumulator {
    QVector<quint64> luminance;
    QVector<quint64> red;
    QVector<quint64> green;
    QVector<quint64> blue;
    QVector<quint64> alpha;
    quint64 includedWeight = 0;
    quint64 includedPixels = 0;
    quint64 transparentPixels = 0;

    explicit HistogramAccumulator(const int binCount = 0)
    {
        luminance.fill(0, binCount);
        red.fill(0, binCount);
        green.fill(0, binCount);
        blue.fill(0, binCount);
        alpha.fill(0, binCount);
    }

    void mergeInto(HistogramData *destination) const
    {
        if (!destination || destination->binCount != luminance.size()) return;
        for (int index = 0; index < luminance.size(); ++index) {
            destination->luminance[index] += luminance[index];
            destination->red[index] += red[index];
            destination->green[index] += green[index];
            destination->blue[index] += blue[index];
            destination->alpha[index] += alpha[index];
        }
        destination->includedWeight += includedWeight;
        destination->includedPixels += includedPixels;
        destination->transparentPixels += transparentPixels;
    }
};

struct HistogramCacheState {
    QMutex mutex;
    QCache<QString, HistogramData> entries {HistogramCacheKiB};
};

HistogramCacheState &histogramCache()
{
    static HistogramCacheState cache;
    return cache;
}

quint8 snapshotCoverageAt(const SelectionMask::Snapshot &snapshot,
                          const int x,
                          const int y)
{
    if (!snapshot.active || x < 0 || y < 0
        || x >= snapshot.size.width() || y >= snapshot.size.height()) {
        return snapshot.active ? 0 : 255;
    }
    const QPoint tileIndex(x / SelectionMask::TileSize,
                           y / SelectionMask::TileSize);
    const quint64 key = SelectionMask::tileKey(tileIndex);
    const auto iterator = snapshot.tiles.constFind(key);
    if (iterator == snapshot.tiles.cend()) {
        return snapshot.implicitCoverage;
    }
    const int localX = x - tileIndex.x() * SelectionMask::TileSize;
    const int localY = y - tileIndex.y() * SelectionMask::TileSize;
    const int tileWidth = std::min(SelectionMask::TileSize,
                                   snapshot.size.width()
                                       - tileIndex.x() * SelectionMask::TileSize);
    const int tileHeight = std::min(SelectionMask::TileSize,
                                    snapshot.size.height()
                                        - tileIndex.y() * SelectionMask::TileSize);
    const QByteArray &bytes = iterator.value();
    if (localX < 0 || localY < 0 || localX >= tileWidth || localY >= tileHeight
        || bytes.size() != tileWidth * tileHeight) {
        return snapshot.implicitCoverage;
    }
    return static_cast<quint8>(bytes.at(localY * tileWidth + localX));
}

inline int luminance8(const int r, const int g, const int b)
{
    return std::clamp(qRound(r * 0.2126 + g * 0.7152 + b * 0.0722), 0, 255);
}

inline int luminance16(const int r, const int g, const int b)
{
    return static_cast<int>(std::clamp<qint64>(
        qRound64(r * 0.2126 + g * 0.7152 + b * 0.0722), 0, 65535));
}

QImage managedLuminanceReference(const QImage &input,
                                 const ColourProcessingCompatibility compatibility)
{
    if (input.isNull()
        || compatibility != ColourProcessingCompatibility::ManagedV1
        || !input.colorSpace().isValid()
        || input.colorSpace() == QColorSpace(QColorSpace::SRgb)) {
        return input;
    }
    ColourTransformRequest request;
    request.source = ColourSpaceDescriptor::fromQColorSpace(input.colorSpace());
    request.destination = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    request.purpose = ColourTransformPurpose::AdjustmentDomain;
    const auto transform = ColourTransformService::instance().qtTransform(request);
    if (!transform.has_value()) return input;
    QImage converted = input.convertToFormat(input.depth() > 32
        ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    converted.applyColorTransform(*transform);
    if (converted.isNull()) return input;
    converted.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return converted;
}

} // namespace

const QVector<quint64> &HistogramData::channel(const HistogramChannel selected) const
{
    switch (selected) {
    case HistogramChannel::Rgb: return luminance;
    case HistogramChannel::Red: return red;
    case HistogramChannel::Green: return green;
    case HistogramChannel::Blue: return blue;
    case HistogramChannel::Alpha: return alpha;
    }
    return luminance;
}

qint64 HistogramData::estimatedBytes() const
{
    return static_cast<qint64>(luminance.capacity() + red.capacity()
                               + green.capacity() + blue.capacity()
                               + alpha.capacity())
        * static_cast<qint64>(sizeof(quint64));
}

bool HistogramData::isValid() const
{
    return adjustmentFound && !cancelled && binCount > 0
        && luminance.size() == binCount && red.size() == binCount
        && green.size() == binCount && blue.size() == binCount
        && alpha.size() == binCount;
}

QString HistogramRequest::cacheKey() const
{
    return QStringLiteral("%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11x%12/%13x%14/%15")
        .arg(documentSessionId.toString(QUuid::WithoutBraces))
        .arg(adjustmentLayerId.toString(QUuid::WithoutBraces))
        .arg(liveFilterOwnerId.toString(QUuid::WithoutBraces))
        .arg(liveFilterId.toString(QUuid::WithoutBraces))
        .arg(documentRevision)
        .arg(colourStateRevision)
        .arg(static_cast<int>(processingCompatibility))
        .arg(static_cast<int>(scope))
        .arg(scope == HistogramScope::Selection ? selection.revision : 0)
        .arg(source.cacheKey())
        .arg(source.width())
        .arg(source.height())
        .arg(documentSize.width())
        .arg(documentSize.height())
        .arg(source.depth() > 32 ? 16 : 8);
}

HistogramService::HistogramService(QObject *parent)
    : QObject(parent)
{
}

HistogramService::~HistogramService()
{
    cancel();
}

quint64 HistogramService::request(const HistogramRequest &request,
                                  Completion completion)
{
    cancel();
    const quint64 serial = m_requestSerial;
    const QString key = request.cacheKey();
    {
        HistogramCacheState &cache = histogramCache();
        QMutexLocker locker(&cache.mutex);
        if (const HistogramData *cached = cache.entries.object(key)) {
            const HistogramData copy = *cached;
            QTimer::singleShot(0, this, [this, serial, completion = std::move(completion), copy] {
                if (serial == m_requestSerial && completion) {
                    completion(serial, copy);
                }
            });
            return serial;
        }
    }

    m_cancelRequested = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancellation = m_cancelRequested;
    auto *watcher = new QFutureWatcher<HistogramData>(this);
    connect(watcher, &QFutureWatcher<HistogramData>::finished, this,
            [this, watcher, serial, key, completion = std::move(completion)]() mutable {
        const HistogramData result = watcher->result();
        watcher->deleteLater();
        if (serial != m_requestSerial || result.cancelled) {
            return;
        }
        if (result.isValid()) {
            HistogramCacheState &cache = histogramCache();
            QMutexLocker locker(&cache.mutex);
            auto *stored = new HistogramData(result);
            const int cost = std::max(1, static_cast<int>(
                std::min<qint64>(std::numeric_limits<int>::max(),
                                 (stored->estimatedBytes() + 1023) / 1024)));
            cache.entries.insert(key, stored, cost);
        }
        if (completion) {
            completion(serial, result);
        }
    });
    watcher->setFuture(QtConcurrent::run([request, cancellation] {
        return HistogramService::calculate(request, cancellation.get());
    }));
    return serial;
}

void HistogramService::cancel()
{
    ++m_requestSerial;
    if (m_cancelRequested) {
        m_cancelRequested->store(true, std::memory_order_release);
        m_cancelRequested.reset();
    }
}

quint64 HistogramService::activeRequestSerial() const
{
    return m_requestSerial;
}

HistogramData HistogramService::calculate(const HistogramRequest &request,
                                          const std::atomic_bool *cancelRequested)
{
    HistogramData result;
    result.sourceBitDepth = request.source.depth() > 32 ? 16 : 8;
    result.binCount = result.sourceBitDepth == 16 ? 65536 : 256;
    result.selectionRevision = request.selection.revision;
    result.luminance.fill(0, result.binCount);
    result.red.fill(0, result.binCount);
    result.green.fill(0, result.binCount);
    result.blue.fill(0, result.binCount);
    result.alpha.fill(0, result.binCount);

    const auto cancelled = [cancelRequested] {
        return cancelRequested
            && cancelRequested->load(std::memory_order_acquire);
    };
    if (cancelled()) {
        result.cancelled = true;
        return result;
    }

    const bool liveFilterRequest = !request.liveFilterOwnerId.isNull()
        && !request.liveFilterId.isNull();
    const QImage input = liveFilterRequest
        ? ImageProcessor::renderLiveFilterInput(request.source,
                                                request.layers,
                                                request.liveFilterOwnerId,
                                                request.liveFilterId,
                                                request.documentSize,
                                                cancelRequested,
                                                request.processingCompatibility)
        : ImageProcessor::renderAdjustmentInput(request.source,
                                                request.layers,
                                                request.adjustmentLayerId,
                                                request.documentSize,
                                                cancelRequested,
                                                request.processingCompatibility);
    if (input.isNull() || cancelled()) {
        result.cancelled = cancelled();
        return result;
    }
    result.adjustmentFound = true;
    const QImage luminanceReference = managedLuminanceReference(
        input, request.processingCompatibility);

    const bool selectionOnly = request.scope == HistogramScope::Selection
        && request.selection.active;
    const qint64 pixelCount = static_cast<qint64>(input.width()) * input.height();
    const int maximumWorkers = result.sourceBitDepth == 16 ? 6 : 8;
    const int workerCount = pixelCount >= ParallelHistogramPixelThreshold
        ? std::clamp(std::min({input.height(), maximumWorkers,
                               std::max(1, QThread::idealThreadCount())}), 1, maximumWorkers)
        : 1;
    QVector<HistogramAccumulator> accumulators;
    accumulators.reserve(workerCount);
    for (int index = 0; index < workerCount; ++index) {
        accumulators.push_back(HistogramAccumulator(result.binCount));
    }
    // Detach the container once on the caller thread. Workers then touch only
    // their own accumulator through this stable storage and never invoke a
    // potentially detaching QVector accessor concurrently.
    HistogramAccumulator *const accumulatorData = accumulators.data();
    std::atomic_bool workerCancelled {false};

    if (result.sourceBitDepth == 16) {
        const QImage rgba = input.convertToFormat(QImage::Format_RGBA64);
        const QImage luminanceRgba = luminanceReference.convertToFormat(
            QImage::Format_RGBA64);
        const auto processWorker = [&](const int worker) {
            HistogramAccumulator &local = accumulatorData[worker];
            const int firstRow = rgba.height() * worker / workerCount;
            const int lastRow = rgba.height() * (worker + 1) / workerCount;
            for (int y = firstRow; y < lastRow; ++y) {
                if (cancelled()) {
                    workerCancelled.store(true, std::memory_order_release);
                    return;
                }
                const auto *row = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(y));
                const auto *luminanceRow = reinterpret_cast<const QRgba64 *>(
                    luminanceRgba.constScanLine(y));
                for (int x = 0; x < rgba.width(); ++x) {
                    const quint64 weight = selectionOnly
                        ? snapshotCoverageAt(request.selection, x, y)
                        : 255;
                    if (weight == 0) continue;
                    const QRgba64 pixel = row[x];
                    local.alpha[static_cast<int>(pixel.alpha())] += weight;
                    if (pixel.alpha() == 0) {
                        ++local.transparentPixels;
                        continue;
                    }
                    ++local.includedPixels;
                    local.includedWeight += weight;
                    local.red[static_cast<int>(pixel.red())] += weight;
                    local.green[static_cast<int>(pixel.green())] += weight;
                    local.blue[static_cast<int>(pixel.blue())] += weight;
                    const QRgba64 luminancePixel = luminanceRow[x];
                    local.luminance[luminance16(luminancePixel.red(),
                                                luminancePixel.green(),
                                                luminancePixel.blue())] += weight;
                }
            }
        };
        if (workerCount == 1) {
            processWorker(0);
        } else {
            QVector<int> workers(workerCount);
            std::iota(workers.begin(), workers.end(), 0);
            QtConcurrent::blockingMap(histogramWorkerPool(), workers, processWorker);
        }
    } else {
        const QImage rgba = input.convertToFormat(QImage::Format_RGBA8888);
        const QImage luminanceRgba = luminanceReference.convertToFormat(
            QImage::Format_RGBA8888);
        const auto processWorker = [&](const int worker) {
            HistogramAccumulator &local = accumulatorData[worker];
            const int firstRow = rgba.height() * worker / workerCount;
            const int lastRow = rgba.height() * (worker + 1) / workerCount;
            for (int y = firstRow; y < lastRow; ++y) {
                if (cancelled()) {
                    workerCancelled.store(true, std::memory_order_release);
                    return;
                }
                const uchar *row = rgba.constScanLine(y);
                const uchar *luminanceRow = luminanceRgba.constScanLine(y);
                for (int x = 0; x < rgba.width(); ++x) {
                    const quint64 weight = selectionOnly
                        ? snapshotCoverageAt(request.selection, x, y)
                        : 255;
                    if (weight == 0) continue;
                    const uchar *pixel = row + x * 4;
                    local.alpha[pixel[3]] += weight;
                    if (pixel[3] == 0) {
                        ++local.transparentPixels;
                        continue;
                    }
                    ++local.includedPixels;
                    local.includedWeight += weight;
                    local.red[pixel[0]] += weight;
                    local.green[pixel[1]] += weight;
                    local.blue[pixel[2]] += weight;
                    const uchar *luminancePixel = luminanceRow + x * 4;
                    local.luminance[luminance8(luminancePixel[0],
                                               luminancePixel[1],
                                               luminancePixel[2])] += weight;
                }
            }
        };
        if (workerCount == 1) {
            processWorker(0);
        } else {
            QVector<int> workers(workerCount);
            std::iota(workers.begin(), workers.end(), 0);
            QtConcurrent::blockingMap(histogramWorkerPool(), workers, processWorker);
        }
    }
    if (workerCancelled.load(std::memory_order_acquire) || cancelled()) {
        result.cancelled = true;
        return result;
    }
    for (const HistogramAccumulator &local : std::as_const(accumulators)) {
        local.mergeInto(&result);
    }
    return result;
}

} // namespace vfx
