#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#endif

#include "DisplayColourManagement.h"

#include "OcioIntegration.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScreen>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <mutex>
#include <string>
#include <variant>
#include <type_traits>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <icm.h>
#endif

namespace vfx {
namespace {

constexpr qint64 MinimumDisplayIccBytes = 128;
constexpr qint64 MaximumDisplayIccBytes = 16LL * 1024LL * 1024LL;

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool cancelled(const std::atomic_bool *token)
{
    return token && token->load(std::memory_order_acquire);
}

ColourSpaceDescriptor srgbDescriptor()
{
    return ColourSpaceDescriptor::fromQColorSpace(QColorSpace(QColorSpace::SRgb));
}

std::optional<ColourSpaceDescriptor> loadIccDescriptor(const QString &path,
                                                       QString *errorMessage)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        setError(errorMessage, QStringLiteral("The monitor ICC profile does not exist: %1").arg(path));
        return std::nullopt;
    }
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, QStringLiteral("The monitor ICC profile could not be read: %1").arg(path));
        return std::nullopt;
    }
    if (file.size() < MinimumDisplayIccBytes || file.size() > MaximumDisplayIccBytes) {
        setError(errorMessage,
                 QStringLiteral("The monitor ICC profile is smaller than an ICC header or exceeds the 16 MiB safety limit: %1")
                     .arg(path));
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size()) {
        setError(errorMessage,
                 QStringLiteral("The monitor ICC profile could not be read completely: %1")
                     .arg(path));
        return std::nullopt;
    }
    const QColorSpace space = QColorSpace::fromIccProfile(bytes);
    if (!space.isValid()) {
        setError(errorMessage, QStringLiteral("The selected file is not a supported RGB or greyscale ICC profile."));
        return std::nullopt;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(bytes);
    return ColourSpaceDescriptor::externalIcc(
        info.canonicalFilePath(), hash.result(), bytes,
        space.description().isEmpty() ? info.completeBaseName() : space.description());
}

MonitorProfileInfo fallbackMonitor(const QScreen *screen, const QString &reason)
{
    MonitorProfileInfo result;
    result.profile = srgbDescriptor();
    result.screenName = screen ? screen->name() : QStringLiteral("Unknown screen");
    result.sourceLabel = QStringLiteral("sRGB fallback");
    result.status = reason.isEmpty()
        ? QStringLiteral("No reliable monitor ICC profile was available; using sRGB.")
        : reason;
    result.source = MonitorProfileSource::SRgbFallback;
    result.fallback = true;
    return result;
}

#ifndef Q_OS_WIN

struct ColordDevice {
    QString objectPath;
    QString deviceId;
    QString model;
    QString vendor;
    QString serial;
    QString xrandrName;
};

QString fieldValue(const QString &line)
{
    const qsizetype colon = line.indexOf(QLatin1Char(':'));
    return colon >= 0 ? line.mid(colon + 1).trimmed() : QString();
}

QVector<ColordDevice> parseColordDevices(const QString &output)
{
    QVector<ColordDevice> devices;
    ColordDevice current;
    const auto finish = [&] {
        if (!current.objectPath.isEmpty() || !current.deviceId.isEmpty()) {
            devices.push_back(current);
            current = {};
        }
    };
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            finish();
            continue;
        }
        if (line.startsWith(QStringLiteral("Object Path:"), Qt::CaseInsensitive)) {
            if (!current.objectPath.isEmpty()) finish();
            current.objectPath = fieldValue(line);
        } else if (line.startsWith(QStringLiteral("Device ID:"), Qt::CaseInsensitive)) {
            current.deviceId = fieldValue(line);
        } else if (line.startsWith(QStringLiteral("Model:"), Qt::CaseInsensitive)) {
            current.model = fieldValue(line);
        } else if (line.startsWith(QStringLiteral("Vendor:"), Qt::CaseInsensitive)) {
            current.vendor = fieldValue(line);
        } else if (line.startsWith(QStringLiteral("Serial:"), Qt::CaseInsensitive)) {
            current.serial = fieldValue(line);
        } else if (line.contains(QStringLiteral("XRANDR_name"), Qt::CaseInsensitive)) {
            const qsizetype equal = line.indexOf(QLatin1Char('='));
            current.xrandrName = equal >= 0 ? line.mid(equal + 1).trimmed() : fieldValue(line);
        }
    }
    finish();
    return devices;
}

QString runColormgr(const QStringList &arguments)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("colormgr"), arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(800)) return {};
    if (!process.waitForFinished(2500)) {
        process.kill();
        process.waitForFinished(250);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

QString parseColordProfileFilename(const QString &output)
{
    for (const QString &raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("Filename:"), Qt::CaseInsensitive)) {
            return fieldValue(line);
        }
    }
    return {};
}

std::optional<QString> colordProfileForScreen(const QScreen *screen)
{
    if (!screen) return std::nullopt;
    const QString output = runColormgr({QStringLiteral("get-devices-by-kind"),
                                       QStringLiteral("display")});
    if (output.isEmpty()) return std::nullopt;
    const QVector<ColordDevice> devices = parseColordDevices(output);
    if (devices.isEmpty()) return std::nullopt;

    const QString screenName = screen->name();
    const QString model = screen->model();
    const QString manufacturer = screen->manufacturer();
    const QString serial = screen->serialNumber();
    int bestScore = -1;
    const ColordDevice *best = nullptr;
    for (const ColordDevice &device : devices) {
        int score = 0;
        if (!screenName.isEmpty() && device.xrandrName.compare(screenName, Qt::CaseInsensitive) == 0) score += 100;
        if (!serial.isEmpty() && device.serial.compare(serial, Qt::CaseInsensitive) == 0) score += 30;
        if (!model.isEmpty() && device.model.contains(model, Qt::CaseInsensitive)) score += 20;
        if (!manufacturer.isEmpty() && device.vendor.contains(manufacturer, Qt::CaseInsensitive)) score += 10;
        if (devices.size() == 1) score += 5;
        if (score > bestScore) {
            bestScore = score;
            best = &device;
        }
    }
    if (!best || bestScore <= 0) return std::nullopt;
    const QString selector = !best->deviceId.isEmpty() ? best->deviceId : best->objectPath;
    const QString profileOutput = runColormgr(
        {QStringLiteral("device-get-default-profile"), selector});
    const QString filename = parseColordProfileFilename(profileOutput);
    if (!filename.isEmpty()) return filename;
    return std::nullopt;
}

#endif // !Q_OS_WIN

#ifdef Q_OS_WIN
std::optional<QString> windowsProfileForScreen(const QScreen *screen)
{
    if (!screen) return std::nullopt;
    const std::wstring device = screen->name().toStdWString();
    const auto profileForScope = [&](const WCS_PROFILE_MANAGEMENT_SCOPE scope)
        -> std::optional<std::wstring> {
        DWORD size = 0;
        if (!WcsGetDefaultColorProfileSize(
                scope, device.c_str(), CPT_ICC, CPST_NONE, 0, &size)
            || size == 0) {
            return std::nullopt;
        }
        std::wstring profile(static_cast<size_t>(size), L'\0');
        if (!WcsGetDefaultColorProfile(
                scope, device.c_str(), CPT_ICC, CPST_NONE, 0,
                size, profile.data())) {
            return std::nullopt;
        }
        if (const size_t terminator = profile.find(L'\0');
            terminator != std::wstring::npos) {
            profile.resize(terminator);
        }
        if (profile.empty()) return std::nullopt;
        return profile;
    };

    auto profile = profileForScope(WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER);
    if (!profile) {
        profile = profileForScope(WCS_PROFILE_MANAGEMENT_SCOPE_SYSTEM_WIDE);
    }
    if (!profile) return std::nullopt;

    QString path = QString::fromStdWString(*profile);
    if (QFileInfo(path).isRelative()) {
        DWORD directorySize = 0;
        GetColorDirectoryW(nullptr, nullptr, &directorySize);
        if (directorySize > 0) {
            std::wstring directory(static_cast<size_t>(directorySize), L'\0');
            if (GetColorDirectoryW(nullptr, directory.data(), &directorySize)) {
                if (const size_t terminator = directory.find(L'\0');
                    terminator != std::wstring::npos) {
                    directory.resize(terminator);
                }
                path = QDir(QString::fromStdWString(directory)).filePath(path);
            }
        }
    }
    return path;
}
#endif

struct QtStage { QColorTransform transform; };
struct OcioStage { std::shared_ptr<const OcioCpuTransform> transform; };
struct OcioDisplayStage { std::shared_ptr<const OcioDisplayTransform> transform; };
using Stage = std::variant<QtStage, OcioStage, OcioDisplayStage>;

struct DisplayGpuLutCpuCache {
    struct Entry {
        std::shared_ptr<const DisplayGpuLutData> data;
        qsizetype bytes = 0;
        quint64 lastUseSerial = 0;
    };

    std::mutex mutex;
    QHash<QByteArray, Entry> entries;
    QHash<QByteArray, QString> rejected;
    qsizetype bytes = 0;
    qsizetype budget = qsizetype(48) * 1024 * 1024;
    quint64 useSerial = 0;

    std::shared_ptr<const DisplayGpuLutData> lookup(const QByteArray &key)
    {
        std::lock_guard lock(mutex);
        auto iterator = entries.find(key);
        if (iterator == entries.end()) return {};
        iterator.value().lastUseSerial = ++useSerial;
        return iterator->data;
    }

    bool lookupRejection(const QByteArray &key, QString *message)
    {
        std::lock_guard lock(mutex);
        const auto iterator = rejected.constFind(key);
        if (iterator == rejected.cend()) return false;
        if (message) *message = iterator.value();
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
                std::shared_ptr<const DisplayGpuLutData> data)
    {
        if (key.isEmpty() || !data) return;
        std::lock_guard lock(mutex);
        auto existing = entries.find(key);
        if (existing != entries.end()) {
            bytes -= existing->bytes;
            entries.erase(existing);
        }
        Entry entry;
        entry.data = std::move(data);
        entry.bytes = (entry.data->forwardRgba16f.size()
                       + entry.data->gamutRoundTripRgba16f.size())
            * qsizetype(sizeof(qfloat16));
        entry.lastUseSerial = ++useSerial;
        bytes += entry.bytes;
        rejected.remove(key);
        entries.insert(key, std::move(entry));
        while (bytes > budget && entries.size() > 1) {
            auto victim = entries.cbegin();
            for (auto iterator = entries.cbegin(); iterator != entries.cend();
                 ++iterator) {
                if (iterator.value().lastUseSerial < victim.value().lastUseSerial) {
                    victim = iterator;
                }
            }
            const QByteArray victimKey = victim.key();
            bytes -= victim.value().bytes;
            entries.remove(victimKey);
        }
    }
};

DisplayGpuLutCpuCache &displayGpuLutCpuCache()
{
    // Deliberately process-lifetime: transforms may be released during Qt
    // shutdown after other function-local statics have already begun teardown.
    static auto *cache = new DisplayGpuLutCpuCache;
    return *cache;
}

bool appendConversionStage(QVector<Stage> *stages,
                           const OcioConfigReference &config,
                           const ColourSpaceDescriptor &source,
                           const ColourSpaceDescriptor &destination,
                           ColourTransformPurpose purpose,
                           ColourRenderingIntent intent,
                           bool blackPointCompensation,
                           QString *errorMessage)
{
    if (!stages) return false;
    if (source.stableFingerprint() == destination.stableFingerprint()) return true;
    if (source.kind == ColourSpaceKind::Ocio || destination.kind == ColourSpaceKind::Ocio) {
        const auto transform = createOcioCpuTransform(config, source, destination, errorMessage);
        if (!transform) return false;
        stages->push_back(OcioStage{transform});
        return true;
    }
    ColourTransformRequest request;
    request.source = source;
    request.destination = destination;
    request.purpose = purpose;
    request.renderingIntent = intent;
    request.blackPointCompensation = blackPointCompensation;
    const auto transform = ColourTransformService::instance().qtTransform(request);
    if (!transform) {
        setError(errorMessage,
                 QStringLiteral("Qt could not create the requested ICC colour transform."));
        return false;
    }
    stages->push_back(QtStage{*transform});
    return true;
}

bool applyStages(QImage *image,
                 const QVector<Stage> &stages,
                 const std::atomic_bool *cancelRequested,
                 QString *errorMessage)
{
    for (const Stage &stage : stages) {
        if (cancelled(cancelRequested)) return false;
        const bool ok = std::visit([&](const auto &typed) -> bool {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, QtStage>) {
                image->applyColorTransform(typed.transform);
                return true;
            } else if constexpr (std::is_same_v<T, OcioStage>) {
                return applyOcioCpuTransform(image, *typed.transform,
                                             cancelRequested, errorMessage);
            } else {
                return applyOcioDisplayTransform(image, *typed.transform,
                                                 cancelRequested, errorMessage);
            }
        }, stage);
        if (!ok) return false;
    }
    return true;
}

void applyGamutWarning(QImage *output,
                       const QImage &source,
                       const QVector<Stage> &roundTripStages,
                       const std::atomic_bool *cancelRequested)
{
    if (!output || output->isNull() || roundTripStages.isEmpty()) return;
    QImage roundTrip = source;
    QString ignored;
    if (!applyStages(&roundTrip, roundTripStages, cancelRequested, &ignored)) return;
    QImage original8 = source.convertToFormat(QImage::Format_RGBA8888);
    QImage roundTrip8 = roundTrip.convertToFormat(QImage::Format_RGBA8888);
    QImage out8 = output->convertToFormat(QImage::Format_RGBA8888);
    if (original8.size() != roundTrip8.size() || original8.size() != out8.size()) return;
    constexpr int threshold = 14;
    for (int y = 0; y < out8.height(); ++y) {
        if (cancelled(cancelRequested)) return;
        const uchar *a = original8.constScanLine(y);
        const uchar *b = roundTrip8.constScanLine(y);
        uchar *out = out8.scanLine(y);
        for (int x = 0; x < out8.width(); ++x) {
            const int o = x * 4;
            const int delta = std::max({std::abs(int(a[o]) - int(b[o])),
                                        std::abs(int(a[o + 1]) - int(b[o + 1])),
                                        std::abs(int(a[o + 2]) - int(b[o + 2]))});
            if (delta > threshold) {
                out[o] = 255;
                out[o + 1] = 0;
                out[o + 2] = 255;
            }
        }
    }
    const QImage::Format originalFormat = output->format();
    if (out8.format() != originalFormat) out8 = out8.convertToFormat(originalFormat);
    out8.setColorSpace(output->colorSpace());
    *output = std::move(out8);
}

std::array<float, 3> sampleDisplayGpuLut(
    const QVector<qfloat16> &values,
    const int edge,
    const std::array<float, 3> input)
{
    const int highest = edge - 1;
    std::array<int, 3> lower {};
    std::array<int, 3> upper {};
    std::array<float, 3> amount {};
    for (int component = 0; component < 3; ++component) {
        const float scaled = std::clamp(input[component], 0.0f, 1.0f)
            * static_cast<float>(highest);
        lower[component] = static_cast<int>(std::floor(scaled));
        upper[component] = std::min(lower[component] + 1, highest);
        amount[component] = scaled - static_cast<float>(lower[component]);
    }
    const auto fetch = [&](const int red, const int green, const int blue) {
        const qsizetype texel = qsizetype(red)
            + qsizetype(blue) * edge
            + qsizetype(green) * edge * edge;
        const qsizetype offset = texel * 4;
        return std::array<float, 3> {
            static_cast<float>(values[offset]),
            static_cast<float>(values[offset + 1]),
            static_cast<float>(values[offset + 2])};
    };
    const auto mix = [](const std::array<float, 3> &left,
                        const std::array<float, 3> &right,
                        const float t) {
        return std::array<float, 3> {
            left[0] + (right[0] - left[0]) * t,
            left[1] + (right[1] - left[1]) * t,
            left[2] + (right[2] - left[2]) * t};
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

int validateDisplayGpuLut(const DisplayGpuLutData &lut,
                          const QVector<Stage> &stages,
                          const QVector<Stage> &roundTripStages,
                          QString *errorMessage)
{
    constexpr int ProbeEdge = 17;
    QImage probe(ProbeEdge * ProbeEdge, ProbeEdge,
                 QImage::Format_RGBA8888);
    if (probe.isNull()) {
        setError(errorMessage,
                 QStringLiteral("The display-transform validation probe could not be allocated."));
        return -1;
    }
    for (int green = 0; green < ProbeEdge; ++green) {
        uchar *row = probe.scanLine(green);
        const int g = qRound(double(green) * 255.0 / double(ProbeEdge - 1));
        for (int blue = 0; blue < ProbeEdge; ++blue) {
            const int b = qRound(double(blue) * 255.0 / double(ProbeEdge - 1));
            for (int red = 0; red < ProbeEdge; ++red) {
                const int r = qRound(double(red) * 255.0 / double(ProbeEdge - 1));
                const int offset = (red + blue * ProbeEdge) * 4;
                row[offset] = static_cast<uchar>(r);
                row[offset + 1] = static_cast<uchar>(g);
                row[offset + 2] = static_cast<uchar>(b);
                row[offset + 3] = static_cast<uchar>(
                    (red * 37 + green * 19 + blue * 11) & 255);
            }
        }
    }

    QImage reference = probe;
    QString stageError;
    if (!applyStages(&reference, stages, nullptr, &stageError)) {
        setError(errorMessage, stageError.isEmpty()
            ? QStringLiteral("The CPU reference could not validate the display-transform lattice.")
            : stageError);
        return -1;
    }
    if (lut.gamutWarning) {
        applyGamutWarning(&reference, probe, roundTripStages, nullptr);
    }
    reference = reference.convertToFormat(QImage::Format_RGBA8888);
    if (reference.isNull()) {
        setError(errorMessage,
                 QStringLiteral("The CPU display-transform validation result is unavailable."));
        return -1;
    }

    int maximumDifference = 0;
    for (int y = 0; y < probe.height(); ++y) {
        const uchar *inputRow = probe.constScanLine(y);
        const uchar *referenceRow = reference.constScanLine(y);
        for (int x = 0; x < probe.width(); ++x) {
            const int offset = x * 4;
            const std::array<float, 3> input {
                inputRow[offset] / 255.0f,
                inputRow[offset + 1] / 255.0f,
                inputRow[offset + 2] / 255.0f};
            auto mapped = sampleDisplayGpuLut(
                lut.forwardRgba16f, lut.edgeSize, input);
            if (lut.gamutWarning) {
                const auto roundTrip = sampleDisplayGpuLut(
                    lut.gamutRoundTripRgba16f, lut.edgeSize, input);
                const auto code = [](const float value) {
                    return std::clamp(
                        static_cast<int>(std::lround(
                            std::clamp(value, 0.0f, 1.0f) * 255.0f)),
                        0, 255);
                };
                const int delta = std::max({
                    std::abs(int(inputRow[offset]) - code(roundTrip[0])),
                    std::abs(int(inputRow[offset + 1]) - code(roundTrip[1])),
                    std::abs(int(inputRow[offset + 2]) - code(roundTrip[2]))});
                if (delta > static_cast<int>(lut.gamutWarningThreshold)) {
                    mapped = {1.0f, 0.0f, 1.0f};
                }
            }
            for (int component = 0; component < 3; ++component) {
                const int quantised = std::clamp(
                    static_cast<int>(std::lround(
                        std::clamp(mapped[component], 0.0f, 1.0f) * 255.0f)),
                    0, 255);
                maximumDifference = std::max(
                    maximumDifference,
                    std::abs(quantised
                             - int(referenceRow[offset + component])));
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

QByteArray MonitorProfileInfo::stableFingerprint() const
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("VFXPhotoLab/MonitorProfileInfo/v1"));
    hash.addData(profile.stableFingerprint());
    hash.addData(screenName.toUtf8());
    hash.addData(profilePath.toUtf8());
    hash.addData(QByteArray::number(static_cast<int>(source)));
    return hash.result();
}

MonitorProfileInfo discoverMonitorProfile(const QScreen *screen,
                                          const QString &manualOverridePath)
{
    if (!manualOverridePath.trimmed().isEmpty()) {
        QString error;
        const auto descriptor = loadIccDescriptor(manualOverridePath, &error);
        if (descriptor) {
            MonitorProfileInfo result;
            result.profile = *descriptor;
            result.screenName = screen ? screen->name() : QStringLiteral("Unknown screen");
            result.profilePath = QFileInfo(manualOverridePath).canonicalFilePath();
            result.sourceLabel = QStringLiteral("Manual override");
            result.status = QStringLiteral("Using the manually selected monitor ICC profile.");
            result.source = MonitorProfileSource::ManualOverride;
            result.detected = true;
            result.reliable = true;
            result.fallback = false;
            return result;
        }
        return fallbackMonitor(screen,
            QStringLiteral("The manual monitor profile is unavailable or invalid; using sRGB. %1").arg(error));
    }

    const QString environmentPath = qEnvironmentVariable("VFXPHOTOLAB_MONITOR_ICC");
    if (!environmentPath.trimmed().isEmpty()) {
        QString error;
        const auto descriptor = loadIccDescriptor(environmentPath, &error);
        if (descriptor) {
            MonitorProfileInfo result;
            result.profile = *descriptor;
            result.screenName = screen ? screen->name() : QStringLiteral("Unknown screen");
            result.profilePath = QFileInfo(environmentPath).canonicalFilePath();
            result.sourceLabel = QStringLiteral("Environment override");
            result.status = QStringLiteral("Using VFXPHOTOLAB_MONITOR_ICC.");
            result.source = MonitorProfileSource::Environment;
            result.detected = true;
            result.reliable = true;
            result.fallback = false;
            return result;
        }
    }

#ifdef Q_OS_WIN
    if (const auto path = windowsProfileForScreen(screen)) {
        QString error;
        if (const auto descriptor = loadIccDescriptor(*path, &error)) {
            MonitorProfileInfo result;
            result.profile = *descriptor;
            result.screenName = screen ? screen->name() : QStringLiteral("Unknown screen");
            result.profilePath = QFileInfo(*path).canonicalFilePath();
            result.sourceLabel = QStringLiteral("Windows Color System");
            result.status = QStringLiteral("Monitor profile discovered through Windows Color System.");
            result.source = MonitorProfileSource::WindowsWcs;
            result.detected = true;
            result.reliable = true;
            result.fallback = false;
            return result;
        }
    }
#else
    if (const auto path = colordProfileForScreen(screen)) {
        QString error;
        if (const auto descriptor = loadIccDescriptor(*path, &error)) {
            MonitorProfileInfo result;
            result.profile = *descriptor;
            result.screenName = screen ? screen->name() : QStringLiteral("Unknown screen");
            result.profilePath = QFileInfo(*path).canonicalFilePath();
            result.sourceLabel = QStringLiteral("colord");
            result.status = QStringLiteral("Monitor profile discovered through colord.");
            result.source = MonitorProfileSource::Colord;
            result.detected = true;
            result.reliable = true;
            result.fallback = false;
            return result;
        }
    }
#endif
    return fallbackMonitor(screen,
        QStringLiteral("No reliable monitor ICC profile was detected for this screen; using sRGB."));
}

struct DisplayColourTransform::Impl {
    QVector<Stage> stages;
    QVector<Stage> gamutRoundTripStages;
    DisplayColourTransformStatus status;
    bool identity = true;
    mutable std::mutex gpuLutMutex;
    mutable std::shared_ptr<const DisplayGpuLutData> gpuLut;
    mutable QString gpuLutError;
    mutable bool gpuLutAttempted = false;
};

DisplayColourTransform::DisplayColourTransform(std::shared_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

DisplayColourTransform::~DisplayColourTransform() = default;

bool DisplayColourTransform::isIdentity() const
{
    return !m_impl || m_impl->identity;
}

QByteArray DisplayColourTransform::fingerprint() const
{
    return m_impl ? m_impl->status.fingerprint : QByteArray();
}

DisplayColourTransformStatus DisplayColourTransform::status() const
{
    return m_impl ? m_impl->status : DisplayColourTransformStatus{};
}

bool DisplayColourTransform::apply(QImage *image,
                                   const std::atomic_bool *cancelRequested,
                                   QString *errorMessage) const
{
    if (!image || image->isNull() || !m_impl || m_impl->identity) return true;
    const QImage source = *image;
    if (!applyStages(image, m_impl->stages, cancelRequested, errorMessage)) return false;
    if (m_impl->status.gamutWarningActive) {
        applyGamutWarning(image, source, m_impl->gamutRoundTripStages, cancelRequested);
    }
    // The resulting RGB values are already encoded for the selected monitor
    // profile or OCIO view. Leave the presentation copy untagged so Qt does
    // not attempt a second colour-space conversion while painting the widget.
    image->setColorSpace({});
    return true;
}

std::shared_ptr<const DisplayGpuLutData> DisplayColourTransform::gpuLutData(
    QString *errorMessage) const
{
    if (errorMessage) errorMessage->clear();
    if (!m_impl || m_impl->identity || m_impl->stages.isEmpty()) {
        return {};
    }

    std::lock_guard lock(m_impl->gpuLutMutex);
    if (m_impl->gpuLutAttempted) {
        if (!m_impl->gpuLut && errorMessage) {
            *errorMessage = m_impl->gpuLutError;
        }
        return m_impl->gpuLut;
    }
    m_impl->gpuLutAttempted = true;

    // A 65^3 half-float lattice keeps the payload bounded (~2.1 MiB per
    // transform) while providing stable sub-code-value parity for ordinary
    // monitor/proof transforms. The CPU chain remains authoritative and is
    // used both to bake this lattice and whenever runtime parity rejects GPU.
    constexpr int Edge = 65;
    QCryptographicHash cacheKeyHash(QCryptographicHash::Sha256);
    cacheKeyHash.addData(QByteArrayLiteral("VFXPhotoLab/DisplayGpuLutData/v1"));
    cacheKeyHash.addData(m_impl->status.fingerprint);
    cacheKeyHash.addData(QByteArray::number(Edge));
    cacheKeyHash.addData(m_impl->status.gamutWarningActive
                             && !m_impl->gamutRoundTripStages.isEmpty()
                         ? QByteArrayLiteral("gamut")
                         : QByteArrayLiteral("ordinary"));
    const QByteArray cacheKey = cacheKeyHash.result();
    if (const auto cached = displayGpuLutCpuCache().lookup(cacheKey)) {
        m_impl->gpuLut = cached;
        return m_impl->gpuLut;
    }
    if (displayGpuLutCpuCache().lookupRejection(
            cacheKey, &m_impl->gpuLutError)) {
        if (errorMessage) *errorMessage = m_impl->gpuLutError;
        return {};
    }
    const auto rejectLut = [&](const QString &message) {
        m_impl->gpuLutError = message;
        displayGpuLutCpuCache().reject(cacheKey, message);
        if (errorMessage) *errorMessage = message;
    };

    const int width = Edge * Edge;
    const int height = Edge;
    QImage lattice(width, height, QImage::Format_RGBA64);
    if (lattice.isNull()) {
        m_impl->gpuLutError = QStringLiteral(
            "The display-transform GPU lattice could not be allocated.");
        if (errorMessage) *errorMessage = m_impl->gpuLutError;
        return {};
    }
    for (int green = 0; green < Edge; ++green) {
        auto *row = reinterpret_cast<QRgba64 *>(lattice.scanLine(green));
        const quint16 g = static_cast<quint16>(
            std::lround(double(green) * 65535.0 / double(Edge - 1)));
        for (int blue = 0; blue < Edge; ++blue) {
            const quint16 b = static_cast<quint16>(
                std::lround(double(blue) * 65535.0 / double(Edge - 1)));
            for (int red = 0; red < Edge; ++red) {
                const quint16 r = static_cast<quint16>(
                    std::lround(double(red) * 65535.0 / double(Edge - 1)));
                row[red + blue * Edge] = QRgba64::fromRgba64(r, g, b, 65535);
            }
        }
    }

    QImage forward = lattice;
    QString stageError;
    if (!applyStages(&forward, m_impl->stages, nullptr, &stageError)) {
        rejectLut(stageError.isEmpty()
                      ? QStringLiteral(
                            "The CPU display reference could not bake the GPU lattice.")
                      : stageError);
        return {};
    }
    forward = forward.convertToFormat(QImage::Format_RGBA64);
    if (forward.isNull()) {
        m_impl->gpuLutError = QStringLiteral(
            "The baked display-transform lattice could not be converted to RGBA64.");
        if (errorMessage) *errorMessage = m_impl->gpuLutError;
        return {};
    }

    auto result = std::make_shared<DisplayGpuLutData>();
    result->edgeSize = Edge;
    result->gamutWarning = m_impl->status.gamutWarningActive
        && !m_impl->gamutRoundTripStages.isEmpty();
    result->forwardRgba16f.resize(result->texelCount() * 4);
    for (int green = 0; green < Edge; ++green) {
        const auto *row = reinterpret_cast<const QRgba64 *>(
            forward.constScanLine(green));
        for (int blue = 0; blue < Edge; ++blue) {
            for (int red = 0; red < Edge; ++red) {
                const qsizetype texel = qsizetype(red)
                    + qsizetype(blue) * Edge
                    + qsizetype(green) * Edge * Edge;
                const QRgba64 pixel = row[red + blue * Edge];
                const qsizetype offset = texel * 4;
                result->forwardRgba16f[offset] = qfloat16(
                    static_cast<float>(pixel.red() / 65535.0));
                result->forwardRgba16f[offset + 1] = qfloat16(
                    static_cast<float>(pixel.green() / 65535.0));
                result->forwardRgba16f[offset + 2] = qfloat16(
                    static_cast<float>(pixel.blue() / 65535.0));
                result->forwardRgba16f[offset + 3] = qfloat16(1.0f);
            }
        }
    }

    if (result->gamutWarning) {
        QImage roundTrip = lattice;
        stageError.clear();
        if (!applyStages(&roundTrip, m_impl->gamutRoundTripStages, nullptr,
                         &stageError)) {
            rejectLut(stageError.isEmpty()
                          ? QStringLiteral(
                                "The gamut-warning round-trip lattice could not be baked.")
                          : stageError);
            return {};
        }
        roundTrip = roundTrip.convertToFormat(QImage::Format_RGBA64);
        if (roundTrip.isNull()) {
            m_impl->gpuLutError = QStringLiteral(
                "The gamut-warning round-trip lattice could not be converted to RGBA64.");
            if (errorMessage) *errorMessage = m_impl->gpuLutError;
            return {};
        }
        result->gamutRoundTripRgba16f.resize(result->texelCount() * 4);
        for (int green = 0; green < Edge; ++green) {
            const auto *row = reinterpret_cast<const QRgba64 *>(
                roundTrip.constScanLine(green));
            for (int blue = 0; blue < Edge; ++blue) {
                for (int red = 0; red < Edge; ++red) {
                    const qsizetype texel = qsizetype(red)
                        + qsizetype(blue) * Edge
                        + qsizetype(green) * Edge * Edge;
                    const QRgba64 pixel = row[red + blue * Edge];
                    const qsizetype offset = texel * 4;
                    result->gamutRoundTripRgba16f[offset] = qfloat16(
                        static_cast<float>(pixel.red() / 65535.0));
                    result->gamutRoundTripRgba16f[offset + 1] = qfloat16(
                        static_cast<float>(pixel.green() / 65535.0));
                    result->gamutRoundTripRgba16f[offset + 2] = qfloat16(
                        static_cast<float>(pixel.blue() / 65535.0));
                    result->gamutRoundTripRgba16f[offset + 3] = qfloat16(1.0f);
                }
            }
        }
    }

    result->fingerprint = cacheKey;
    if (!result->isValid()) {
        rejectLut(QStringLiteral(
            "The baked display-transform GPU lattice is incomplete."));
        return {};
    }
    QString validationError;
    result->referenceMaximumDifference = validateDisplayGpuLut(
        *result, m_impl->stages, m_impl->gamutRoundTripStages,
        &validationError);
    constexpr int MaximumReferenceDifference = 4;
    if (result->referenceMaximumDifference < 0
        || result->referenceMaximumDifference > MaximumReferenceDifference) {
        rejectLut(validationError.isEmpty()
                      ? QStringLiteral(
                            "The display transform requires the exact CPU reference "
                            "because its 65³ lattice differed by %1 code values (limit %2).")
                            .arg(result->referenceMaximumDifference)
                            .arg(MaximumReferenceDifference)
                      : validationError);
        return {};
    }
    displayGpuLutCpuCache().insert(cacheKey, result);
    m_impl->gpuLut = std::move(result);
    return m_impl->gpuLut;
}

std::shared_ptr<const DisplayColourTransform> createDisplayColourTransform(
    const DocumentColourState &state,
    const MonitorProfileInfo &monitor,
    QString *errorMessage)
{
    auto impl = std::make_shared<DisplayColourTransform::Impl>();
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    fingerprint.addData(QByteArrayLiteral("VFXPhotoLab/DisplayColourTransform/v1"));
    fingerprint.addData(state.stableFingerprint());
    fingerprint.addData(monitor.stableFingerprint());

    if (!state.presentationColourManagementEnabled
        || state.displayTransform.kind == DisplayTransformKind::Disabled) {
        impl->identity = true;
        impl->status.summary = state.presentationColourManagementEnabled
            ? QStringLiteral("Display colour management is disabled for this document.")
            : QStringLiteral("Legacy-compatible presentation is active; stored display settings are not being applied.");
        impl->status.fingerprint = fingerprint.result();
        return std::shared_ptr<const DisplayColourTransform>(
            new DisplayColourTransform(std::move(impl)));
    }

    ColourSpaceDescriptor current = state.workingSpace;
    if (current.isUntagged()) {
        current = srgbDescriptor();
        impl->status.warning = QStringLiteral("The document working space is untagged; sRGB is being used for presentation only.");
    }

    if (state.proofing.enabled) {
        if (!appendConversionStage(&impl->stages, state.ocioConfig,
                                   current, state.proofing.profile,
                                   ColourTransformPurpose::WorkingToProof,
                                   state.proofing.renderingIntent,
                                   state.proofing.blackPointCompensation,
                                   errorMessage)) {
            return {};
        }
        if (state.proofing.gamutWarning) {
            if (appendConversionStage(&impl->gamutRoundTripStages, state.ocioConfig,
                                      current, state.proofing.profile,
                                      ColourTransformPurpose::WorkingToProof,
                                      state.proofing.renderingIntent,
                                      state.proofing.blackPointCompensation,
                                      nullptr)
                && appendConversionStage(&impl->gamutRoundTripStages, state.ocioConfig,
                                         state.proofing.profile, current,
                                         ColourTransformPurpose::WorkingToWorking,
                                         state.proofing.renderingIntent,
                                         state.proofing.blackPointCompensation,
                                         nullptr)) {
                impl->status.gamutWarningActive = true;
            }
        }
        current = state.proofing.profile;
        impl->status.proofingActive = true;
    }

    switch (state.displayTransform.kind) {
    case DisplayTransformKind::SystemIcc:
        if (!appendConversionStage(&impl->stages, state.ocioConfig,
                                   current, monitor.profile,
                                   ColourTransformPurpose::WorkingToDisplay,
                                   ColourRenderingIntent::RelativeColorimetric,
                                   true, errorMessage)) {
            return {};
        }
        impl->status.fallback = monitor.fallback;
        impl->status.summary = QStringLiteral("%1 — %2")
            .arg(monitor.screenName, monitor.profile.displayName);
        break;
    case DisplayTransformKind::IccProfile:
        if (!appendConversionStage(&impl->stages, state.ocioConfig,
                                   current, state.displayTransform.profile,
                                   ColourTransformPurpose::WorkingToDisplay,
                                   ColourRenderingIntent::RelativeColorimetric,
                                   true, errorMessage)) {
            return {};
        }
        impl->status.summary = QStringLiteral("Document display ICC — %1")
            .arg(state.displayTransform.profile.displayName);
        break;
    case DisplayTransformKind::OcioView: {
        const auto display = createOcioDisplayTransform(
            state.ocioConfig, current,
            state.displayTransform.ocioDisplay,
            state.displayTransform.ocioView,
            state.displayTransform.ocioLook,
            errorMessage);
        if (!display) return {};
        impl->stages.push_back(OcioDisplayStage{display});
        impl->status.summary = QStringLiteral("OCIO %1 / %2")
            .arg(state.displayTransform.ocioDisplay,
                 state.displayTransform.ocioView);
        break;
    }
    case DisplayTransformKind::Disabled:
        break;
    }

    impl->identity = impl->stages.isEmpty();
    impl->status.active = !impl->identity;
    if (impl->status.proofingActive) {
        impl->status.summary += QStringLiteral("; proofing %1 (%2%3)")
            .arg(state.proofing.profile.displayName,
                 colourRenderingIntentName(state.proofing.renderingIntent),
                 state.proofing.blackPointCompensation
                     ? QStringLiteral(", BPC requested") : QString());
    }
    impl->status.fingerprint = fingerprint.result();
    return std::shared_ptr<const DisplayColourTransform>(
        new DisplayColourTransform(std::move(impl)));
}

QString colourRenderingIntentName(const ColourRenderingIntent intent)
{
    switch (intent) {
    case ColourRenderingIntent::Perceptual: return QStringLiteral("Perceptual");
    case ColourRenderingIntent::RelativeColorimetric: return QStringLiteral("Relative colorimetric");
    case ColourRenderingIntent::Saturation: return QStringLiteral("Saturation");
    case ColourRenderingIntent::AbsoluteColorimetric: return QStringLiteral("Absolute colorimetric");
    }
    return QStringLiteral("Relative colorimetric");
}

} // namespace vfx
