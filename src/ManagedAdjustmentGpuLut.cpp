#include "ManagedAdjustmentGpuLut.h"

#include "ColourManagement.h"

#include <QCryptographicHash>
#include <QColorTransform>
#include <QHash>
#include <QImage>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

namespace vfx {
namespace {

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

QColorSpace domainColourSpace(const QColorSpace &workingSpace,
                              const AdjustmentProcessingDomain domain)
{
    if (!workingSpace.isValid()) return {};
    switch (domain) {
    case AdjustmentProcessingDomain::LinearWorking: {
        QColorSpace linear = workingSpace.withTransferFunction(
            QColorSpace::TransferFunction::Linear);
        if (linear.isValid()) {
            linear.setDescription(workingSpace.description().isEmpty()
                ? QStringLiteral("Linear working space")
                : workingSpace.description() + QStringLiteral(" (Linear)"));
        }
        return linear;
    }
    case AdjustmentProcessingDomain::EncodedSrgb:
        return QColorSpace(QColorSpace::SRgb);
    case AdjustmentProcessingDomain::EncodedWorking:
    case AdjustmentProcessingDomain::RawComponents:
    case AdjustmentProcessingDomain::LutContract:
        return workingSpace;
    }
    return workingSpace;
}

struct Cache {
    struct Entry {
        std::shared_ptr<const ManagedAdjustmentGpuLutData> data;
        qsizetype bytes = 0;
        quint64 lastUse = 0;
    };

    std::mutex mutex;
    QHash<QByteArray, Entry> entries;
    QHash<QByteArray, QString> rejected;
    qsizetype bytes = 0;
    qsizetype budget = qsizetype(48) * 1024 * 1024;
    quint64 serial = 0;

    std::shared_ptr<const ManagedAdjustmentGpuLutData> lookup(
        const QByteArray &key)
    {
        std::lock_guard lock(mutex);
        auto iterator = entries.find(key);
        if (iterator == entries.end()) return {};
        iterator.value().lastUse = ++serial;
        return iterator.value().data;
    }

    bool lookupRejection(const QByteArray &key, QString *errorMessage)
    {
        std::lock_guard lock(mutex);
        const auto iterator = rejected.constFind(key);
        if (iterator == rejected.cend()) return false;
        setError(errorMessage, iterator.value());
        return true;
    }

    void reject(const QByteArray &key, const QString &message)
    {
        if (key.isEmpty() || message.isEmpty()) return;
        std::lock_guard lock(mutex);
        if (rejected.size() >= 64 && !rejected.contains(key)) {
            rejected.erase(rejected.begin());
        }
        rejected.insert(key, message);
    }

    void insert(const QByteArray &key,
                std::shared_ptr<const ManagedAdjustmentGpuLutData> data)
    {
        if (key.isEmpty() || !data) return;
        std::lock_guard lock(mutex);
        auto existing = entries.find(key);
        if (existing != entries.end()) {
            bytes -= existing.value().bytes;
            entries.erase(existing);
        }
        Entry entry;
        entry.bytes = (data->workingToDomainRgba16f.size()
                       + data->domainToWorkingRgba16f.size())
            * qsizetype(sizeof(qfloat16));
        entry.data = std::move(data);
        entry.lastUse = ++serial;
        bytes += entry.bytes;
        rejected.remove(key);
        entries.insert(key, std::move(entry));
        while (bytes > budget && entries.size() > 1) {
            auto victim = entries.cbegin();
            for (auto iterator = entries.cbegin(); iterator != entries.cend();
                 ++iterator) {
                if (iterator.value().lastUse < victim.value().lastUse) {
                    victim = iterator;
                }
            }
            const QByteArray victimKey = victim.key();
            bytes -= victim.value().bytes;
            entries.remove(victimKey);
        }
    }
};

Cache &cache()
{
    static auto *instance = new Cache;
    return *instance;
}

QImage latticeImage(const int edge)
{
    QImage lattice(edge * edge, edge, QImage::Format_RGBA64);
    if (lattice.isNull()) return {};
    for (int green = 0; green < edge; ++green) {
        auto *row = reinterpret_cast<QRgba64 *>(lattice.scanLine(green));
        const quint16 g = static_cast<quint16>(
            std::lround(double(green) * 65535.0 / double(edge - 1)));
        for (int blue = 0; blue < edge; ++blue) {
            const quint16 b = static_cast<quint16>(
                std::lround(double(blue) * 65535.0 / double(edge - 1)));
            for (int red = 0; red < edge; ++red) {
                const quint16 r = static_cast<quint16>(
                    std::lround(double(red) * 65535.0 / double(edge - 1)));
                row[red + blue * edge] = QRgba64::fromRgba64(r, g, b, 65535);
            }
        }
    }
    return lattice;
}

QVector<qfloat16> flatten(const QImage &image, const int edge)
{
    QVector<qfloat16> values(qsizetype(edge) * edge * edge * 4);
    for (int green = 0; green < edge; ++green) {
        const auto *row = reinterpret_cast<const QRgba64 *>(
            image.constScanLine(green));
        for (int blue = 0; blue < edge; ++blue) {
            for (int red = 0; red < edge; ++red) {
                const qsizetype texel = qsizetype(red)
                    + qsizetype(blue) * edge
                    + qsizetype(green) * edge * edge;
                const qsizetype offset = texel * 4;
                const QRgba64 pixel = row[red + blue * edge];
                values[offset] = qfloat16(float(pixel.red()) / 65535.0f);
                values[offset + 1] = qfloat16(float(pixel.green()) / 65535.0f);
                values[offset + 2] = qfloat16(float(pixel.blue()) / 65535.0f);
                values[offset + 3] = qfloat16(1.0f);
            }
        }
    }
    return values;
}

std::array<float, 3> sample(const QVector<qfloat16> &values,
                            const int edge,
                            const std::array<float, 3> input)
{
    const int highest = edge - 1;
    std::array<int, 3> lower {};
    std::array<int, 3> upper {};
    std::array<float, 3> amount {};
    for (int component = 0; component < 3; ++component) {
        const float scaled = std::clamp(input[component], 0.0f, 1.0f)
            * float(highest);
        lower[component] = int(std::floor(scaled));
        upper[component] = std::min(lower[component] + 1, highest);
        amount[component] = scaled - float(lower[component]);
    }
    const auto fetch = [&](const int red, const int green, const int blue) {
        const qsizetype texel = qsizetype(red)
            + qsizetype(blue) * edge
            + qsizetype(green) * edge * edge;
        const qsizetype offset = texel * 4;
        return std::array<float, 3> {
            float(values[offset]), float(values[offset + 1]),
            float(values[offset + 2])};
    };
    const auto mix = [](const std::array<float, 3> &left,
                        const std::array<float, 3> &right,
                        const float amount) {
        return std::array<float, 3> {
            left[0] + (right[0] - left[0]) * amount,
            left[1] + (right[1] - left[1]) * amount,
            left[2] + (right[2] - left[2]) * amount};
    };
    const auto c000 = fetch(lower[0], lower[1], lower[2]);
    const auto c100 = fetch(upper[0], lower[1], lower[2]);
    const auto c010 = fetch(lower[0], upper[1], lower[2]);
    const auto c110 = fetch(upper[0], upper[1], lower[2]);
    const auto c001 = fetch(lower[0], lower[1], upper[2]);
    const auto c101 = fetch(upper[0], lower[1], upper[2]);
    const auto c011 = fetch(lower[0], upper[1], upper[2]);
    const auto c111 = fetch(upper[0], upper[1], upper[2]);
    const auto z0 = mix(mix(c000, c100, amount[0]),
                        mix(c010, c110, amount[0]), amount[1]);
    const auto z1 = mix(mix(c001, c101, amount[0]),
                        mix(c011, c111, amount[0]), amount[1]);
    return mix(z0, z1, amount[2]);
}

int validateDirection(const QVector<qfloat16> &values,
                      const int edge,
                      const std::optional<QColorTransform> &transform,
                      QString *errorMessage)
{
    constexpr int ProbeEdge = 17;
    QImage probe(ProbeEdge * ProbeEdge, ProbeEdge, QImage::Format_RGBA8888);
    if (probe.isNull()) {
        setError(errorMessage,
                 QStringLiteral("The managed adjustment validation probe could not be allocated."));
        return -1;
    }
    for (int green = 0; green < ProbeEdge; ++green) {
        uchar *row = probe.scanLine(green);
        const int g = int(std::lround(double(green) * 255.0
                                      / double(ProbeEdge - 1)));
        for (int blue = 0; blue < ProbeEdge; ++blue) {
            const int b = int(std::lround(double(blue) * 255.0
                                          / double(ProbeEdge - 1)));
            for (int red = 0; red < ProbeEdge; ++red) {
                const int r = int(std::lround(double(red) * 255.0
                                              / double(ProbeEdge - 1)));
                const int offset = (red + blue * ProbeEdge) * 4;
                row[offset] = uchar(r);
                row[offset + 1] = uchar(g);
                row[offset + 2] = uchar(b);
                row[offset + 3] = uchar((red * 31 + green * 17 + blue * 7) & 255);
            }
        }
    }
    QImage reference = probe;
    if (transform.has_value()) reference.applyColorTransform(*transform);
    reference = reference.convertToFormat(QImage::Format_RGBA8888);
    if (reference.isNull()) {
        setError(errorMessage,
                 QStringLiteral("The managed adjustment CPU validation result is unavailable."));
        return -1;
    }

    int maximumDifference = 0;
    for (int y = 0; y < probe.height(); ++y) {
        const uchar *inputRow = probe.constScanLine(y);
        const uchar *referenceRow = reference.constScanLine(y);
        for (int x = 0; x < probe.width(); ++x) {
            const int offset = x * 4;
            const auto mapped = sample(values, edge,
                {inputRow[offset] / 255.0f,
                 inputRow[offset + 1] / 255.0f,
                 inputRow[offset + 2] / 255.0f});
            for (int component = 0; component < 3; ++component) {
                const int quantised = std::clamp(int(std::lround(
                    std::clamp(mapped[component], 0.0f, 1.0f) * 255.0f)),
                    0, 255);
                maximumDifference = std::max(
                    maximumDifference,
                    std::abs(quantised - int(referenceRow[offset + component])));
            }
            maximumDifference = std::max(
                maximumDifference,
                std::abs(int(inputRow[offset + 3])
                         - int(referenceRow[offset + 3])));
        }
    }
    return maximumDifference;
}

} // namespace

std::shared_ptr<const ManagedAdjustmentGpuLutData>
createManagedAdjustmentGpuLut(const QColorSpace &workingSpace,
                              const AdjustmentProcessingDomain domain,
                              QString *errorMessage)
{
    if (errorMessage) errorMessage->clear();
    if (domain != AdjustmentProcessingDomain::LinearWorking
        && domain != AdjustmentProcessingDomain::EncodedSrgb) {
        setError(errorMessage,
                 QStringLiteral("This adjustment domain does not require a managed GPU transform."));
        return {};
    }
    if (!workingSpace.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The document working colour space is unavailable."));
        return {};
    }
    const QColorSpace domainSpace = domainColourSpace(workingSpace, domain);
    if (!domainSpace.isValid()) {
        setError(errorMessage,
                 QStringLiteral("The requested managed adjustment domain cannot be represented."));
        return {};
    }

    constexpr int Edge = 65;
    const ColourSpaceDescriptor workingDescriptor =
        ColourSpaceDescriptor::fromQColorSpace(workingSpace);
    const ColourSpaceDescriptor domainDescriptor =
        ColourSpaceDescriptor::fromQColorSpace(domainSpace);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("VFXPhotoLab/ManagedAdjustmentGpuLut/v1"));
    hash.addData(workingDescriptor.stableFingerprint());
    hash.addData(domainDescriptor.stableFingerprint());
    hash.addData(QByteArray::number(static_cast<int>(domain)));
    hash.addData(QByteArray::number(Edge));
    const QByteArray fingerprint = hash.result();
    if (const auto cached = cache().lookup(fingerprint)) return cached;
    if (cache().lookupRejection(fingerprint, errorMessage)) return {};

    const auto reject = [&](const QString &message) {
        setError(errorMessage, message);
        cache().reject(fingerprint, message);
    };

    std::optional<QColorTransform> toDomain;
    std::optional<QColorTransform> toWorking;
    if (workingDescriptor.stableFingerprint()
        != domainDescriptor.stableFingerprint()) {
        ColourTransformRequest forwardRequest;
        forwardRequest.source = workingDescriptor;
        forwardRequest.destination = domainDescriptor;
        forwardRequest.purpose = ColourTransformPurpose::AdjustmentDomain;
        toDomain = ColourTransformService::instance().qtTransform(forwardRequest);
        ColourTransformRequest inverseRequest;
        inverseRequest.source = domainDescriptor;
        inverseRequest.destination = workingDescriptor;
        inverseRequest.purpose = ColourTransformPurpose::AdjustmentDomain;
        toWorking = ColourTransformService::instance().qtTransform(inverseRequest);
        if (!toDomain.has_value() || !toWorking.has_value()) {
            reject(QStringLiteral(
                "Qt could not create the managed adjustment-domain colour transforms."));
            return {};
        }
    }

    const QImage lattice = latticeImage(Edge);
    if (lattice.isNull()) {
        reject(QStringLiteral(
            "The managed adjustment GPU lattice could not be allocated."));
        return {};
    }
    QImage forward = lattice;
    QImage inverse = lattice;
    if (toDomain.has_value()) forward.applyColorTransform(*toDomain);
    if (toWorking.has_value()) inverse.applyColorTransform(*toWorking);
    forward = forward.convertToFormat(QImage::Format_RGBA64);
    inverse = inverse.convertToFormat(QImage::Format_RGBA64);
    if (forward.isNull() || inverse.isNull()) {
        reject(QStringLiteral(
            "The managed adjustment GPU lattice could not be converted to RGBA64."));
        return {};
    }

    auto result = std::make_shared<ManagedAdjustmentGpuLutData>();
    result->edgeSize = Edge;
    result->domain = domain;
    result->fingerprint = fingerprint;
    result->workingToDomainRgba16f = flatten(forward, Edge);
    result->domainToWorkingRgba16f = flatten(inverse, Edge);
    if (!result->isValid()) {
        reject(QStringLiteral("The managed adjustment GPU lattice is incomplete."));
        return {};
    }

    QString validationError;
    const int forwardDifference = validateDirection(
        result->workingToDomainRgba16f, Edge, toDomain, &validationError);
    const int inverseDifference = validateDirection(
        result->domainToWorkingRgba16f, Edge, toWorking, &validationError);
    result->referenceMaximumDifference = std::max(
        forwardDifference, inverseDifference);
    constexpr int MaximumReferenceDifference = 4;
    if (forwardDifference < 0 || inverseDifference < 0
        || result->referenceMaximumDifference > MaximumReferenceDifference) {
        reject(validationError.isEmpty()
                   ? QStringLiteral(
                         "The managed adjustment transform requires the exact CPU reference because its 65³ lattice differed by %1 code values (limit %2).")
                         .arg(result->referenceMaximumDifference)
                         .arg(MaximumReferenceDifference)
                   : validationError);
        return {};
    }

    cache().insert(fingerprint, result);
    return result;
}

} // namespace vfx
