#include "AppStyle.h"
#include "MainWindow.h"
#include "OcioIntegration.h"
#include "gpu/RenderBackend.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QTextStream>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>

namespace {

constexpr auto kGpuSelfTestArgument = "--gpu-self-test";
constexpr auto kPackageSmokeTestArgument = "--package-smoke-test";
constexpr auto kPackageSmokeRequireFullArgument = "--require-full-release";
constexpr int kGpuHelperTimeoutMs = 20000;
constexpr auto kGpuCapabilitiesMarker = "VFXPHOTOLAB_GPU_CAPABILITIES=";

QByteArray encodeGpuCapabilities(
    const vfx::GpuDiagnosticCapabilities &capabilities)
{
    QJsonObject webGpu;
    webGpu.insert(QStringLiteral("foundation"), capabilities.webGpu.foundation);
    webGpu.insert(QStringLiteral("fill"), capabilities.webGpu.fill);
    webGpu.insert(QStringLiteral("gradient"), capabilities.webGpu.gradient);
    webGpu.insert(QStringLiteral("vectorFeather"),
                  capabilities.webGpu.vectorFeather);
    webGpu.insert(QStringLiteral("displayTransform"),
                  capabilities.webGpu.displayTransform);
    webGpu.insert(QStringLiteral("managedAdjustmentTransforms"),
                  capabilities.webGpu.managedAdjustmentTransforms);
    webGpu.insert(QStringLiteral("approvedAdjustmentMask"),
                  QString::number(capabilities.webGpu.approvedAdjustmentMask));

    QJsonObject root;
    root.insert(QStringLiteral("protocol"), 1);
    root.insert(QStringLiteral("valid"), capabilities.valid);
    root.insert(QStringLiteral("foundation"), capabilities.foundation);
    root.insert(QStringLiteral("compositor"), capabilities.compositor);
    root.insert(QStringLiteral("standardCompositor"),
                capabilities.standardCompositor);
    root.insert(QStringLiteral("brush"), capabilities.brush);
    root.insert(QStringLiteral("cloneStamp"), capabilities.cloneStamp);
    root.insert(QStringLiteral("localValidationStatus"),
                capabilities.localValidationStatus);
    root.insert(QStringLiteral("webGpu"), webGpu);
    return QJsonDocument(root).toJson(QJsonDocument::Compact)
        .toBase64(QByteArray::Base64UrlEncoding
                  | QByteArray::OmitTrailingEquals);
}

bool decodeGpuCapabilities(
    const QByteArray &encoded,
    vfx::GpuDiagnosticCapabilities *capabilities)
{
    if (!capabilities || encoded.isEmpty()) {
        return false;
    }
    const QByteArray json = QByteArray::fromBase64(
        encoded, QByteArray::Base64UrlEncoding
                     | QByteArray::AbortOnBase64DecodingErrors);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("protocol")).toInt() != 1) {
        return false;
    }
    const QJsonObject webGpu = root.value(QStringLiteral("webGpu")).toObject();
    if (webGpu.isEmpty()) {
        return false;
    }

    vfx::GpuDiagnosticCapabilities decoded;
    decoded.valid = root.value(QStringLiteral("valid")).toBool(false);
    decoded.foundation = root.value(QStringLiteral("foundation")).toBool(false);
    decoded.compositor = root.value(QStringLiteral("compositor")).toBool(false);
    decoded.standardCompositor =
        root.value(QStringLiteral("standardCompositor")).toBool(false);
    decoded.brush = root.value(QStringLiteral("brush")).toBool(false);
    decoded.cloneStamp = root.value(QStringLiteral("cloneStamp")).toBool(false);
    decoded.localValidationStatus =
        root.value(QStringLiteral("localValidationStatus")).toString();
    decoded.webGpu.foundation =
        webGpu.value(QStringLiteral("foundation")).toBool(false);
    decoded.webGpu.fill = webGpu.value(QStringLiteral("fill")).toBool(false);
    decoded.webGpu.gradient =
        webGpu.value(QStringLiteral("gradient")).toBool(false);
    decoded.webGpu.vectorFeather =
        webGpu.value(QStringLiteral("vectorFeather")).toBool(false);
    decoded.webGpu.displayTransform =
        webGpu.value(QStringLiteral("displayTransform")).toBool(false);
    decoded.webGpu.managedAdjustmentTransforms =
        webGpu.value(QStringLiteral("managedAdjustmentTransforms")).toBool(false);
    bool adjustmentMaskValid = false;
    const qulonglong adjustmentMask =
        webGpu.value(QStringLiteral("approvedAdjustmentMask"))
            .toString().toULongLong(&adjustmentMaskValid);
    if (!adjustmentMaskValid
        || adjustmentMask > std::numeric_limits<quint32>::max()) {
        return false;
    }
    decoded.webGpu.approvedAdjustmentMask =
        static_cast<quint32>(adjustmentMask);
    if (!decoded.valid
        || decoded.foundation != decoded.webGpu.foundation) {
        return false;
    }
    *capabilities = decoded;
    return true;
}

bool hasArgument(const int argc, char *argv[], const char *argument)
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], argument) == 0) {
            return true;
        }
    }
    return false;
}

QString argumentValue(const int argc,
                      char *argv[],
                      const char *argument)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], argument) == 0) {
            return QString::fromLocal8Bit(argv[index + 1]);
        }
    }
    return {};
}

int runPackageSmokeTest(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("VFX Photo Lab Package Smoke Test"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(VFXPHOTOLAB_VERSION));

    const bool requireFullRelease =
        hasArgument(argc, argv, kPackageSmokeRequireFullArgument);
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    const QDir applicationDir(applicationDirectory);
    const auto existsAny = [&applicationDir](const QStringList &relativePaths) {
        return std::any_of(relativePaths.cbegin(),
                           relativePaths.cend(),
                           [&applicationDir](const QString &path) {
            return QFileInfo::exists(applicationDir.filePath(path));
        });
    };

    QStringList imageFormats;
    for (const QByteArray &format : QImageReader::supportedImageFormats()) {
        imageFormats.push_back(QString::fromLatin1(format).toLower());
    }
    imageFormats.removeDuplicates();
    imageFormats.sort();
    const bool jpegAvailable = imageFormats.contains(QStringLiteral("jpeg"))
        || imageFormats.contains(QStringLiteral("jpg"));

#ifdef Q_OS_WIN
    const bool windowsPlatformPlugin = existsAny({
        QStringLiteral("platforms/qwindows.dll"),
        QStringLiteral("plugins/platforms/qwindows.dll")
    });
    const bool jpegPluginFile = existsAny({
        QStringLiteral("imageformats/qjpeg.dll"),
        QStringLiteral("plugins/imageformats/qjpeg.dll")
    });
    const bool wgpuRuntime = existsAny({
        QStringLiteral("wgpu_native.dll"),
        QStringLiteral("wgpu.dll")
    });
#else
    const bool windowsPlatformPlugin = true;
    const bool jpegPluginFile = true;
    const bool wgpuRuntime = true;
#endif

#ifdef VFXPHOTOLAB_PACKAGE_HAS_WEBGPU
    constexpr bool webGpuCompiled = true;
#else
    constexpr bool webGpuCompiled = false;
#endif

    const bool ocioCompiled = vfx::ocioSupportCompiled();
    const QString ocioVersion = vfx::ocioLibraryVersion();
    const bool baseOk = !QCoreApplication::applicationVersion().isEmpty()
        && jpegAvailable
        && windowsPlatformPlugin
        && jpegPluginFile;
    const bool fullReleaseOk = !requireFullRelease
        || (webGpuCompiled && wgpuRuntime && ocioCompiled);
    const bool ok = baseOk && fullReleaseOk;

    QJsonObject root;
    root.insert(QStringLiteral("ok"), ok);
    root.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
    root.insert(QStringLiteral("applicationDirectory"), applicationDirectory);
    root.insert(QStringLiteral("requireFullRelease"), requireFullRelease);
    root.insert(QStringLiteral("jpegAvailable"), jpegAvailable);
    root.insert(QStringLiteral("windowsPlatformPlugin"), windowsPlatformPlugin);
    root.insert(QStringLiteral("jpegPluginFile"), jpegPluginFile);
    root.insert(QStringLiteral("webGpuCompiled"), webGpuCompiled);
    root.insert(QStringLiteral("wgpuRuntime"), wgpuRuntime);
    root.insert(QStringLiteral("ocioCompiled"), ocioCompiled);
    root.insert(QStringLiteral("ocioVersion"), ocioVersion);
    root.insert(QStringLiteral("imageFormats"), imageFormats.join(QLatin1Char(',')));

    const QByteArray report = QJsonDocument(root).toJson(QJsonDocument::Indented);
    const QString jsonPath = argumentValue(argc, argv, "--json");
    if (!jsonPath.isEmpty()) {
        QFile file(jsonPath);
        const QFileInfo fileInfo(file);
        if (!fileInfo.absoluteDir().exists()
            && !QDir().mkpath(fileInfo.absolutePath())) {
            qCritical().noquote() << "Could not create smoke-test report directory:"
                                  << fileInfo.absolutePath();
            return 4;
        }
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(report) != report.size()) {
            qCritical().noquote() << "Could not write smoke-test report:"
                                  << jsonPath;
            return 4;
        }
    }

    QTextStream output(stdout);
    output << report;
    output.flush();
    return ok ? 0 : 3;
}

int runGpuSelfTest(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("VFX Photo Lab GPU Diagnostic"));

    qInfo().noquote() << "[GPU diagnostic] Starting isolated native WebGPU test.";
    vfx::RenderBackend &backend = vfx::RenderBackend::instance();
    const bool passed = backend.initialiseGpuFoundation();
    const QString status = backend.statusText();
    const vfx::GpuDiagnosticCapabilities capabilities =
        backend.diagnosticCapabilities();

    // This process is a disposable crash-isolation probe. Once the stable
    // result has been produced, let the operating system reclaim its native
    // graphics objects rather than entering wgpu-native, Qt and function-static
    // teardown after the diagnostic boundary. The normal GUI process has an
    // explicit orderly GPU shutdown path of its own.
    qInfo().noquote()
        << "[GPU diagnostic] Result complete; leaving native teardown to the disposable helper process boundary.";

    // This stable marker is parsed by the normal GUI parent process. Keep the
    // human-readable stage messages above it for terminal diagnostics.
    QTextStream resultStream(stdout);
    const QByteArray encodedCapabilities = encodeGpuCapabilities(capabilities);
    resultStream << QString::fromLatin1(kGpuCapabilitiesMarker)
                 << QString::fromLatin1(encodedCapabilities.constData(),
                                        encodedCapabilities.size())
                 << QLatin1Char('\n');
    resultStream << QStringLiteral("VFXPHOTOLAB_GPU_RESULT=%1\t%2\n")
                        .arg(passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                             status);
    resultStream.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(passed ? 0 : 2);
}

void startIsolatedGpuDiagnostic(QCoreApplication &application)
{
    auto *process = new QProcess(&application);
    process->setObjectName(QStringLiteral("vfxphotolabGpuDiagnostic"));
    process->setProcessChannelMode(QProcess::MergedChannels);

    auto output = std::make_shared<QByteArray>();
    auto timedOut = std::make_shared<bool>(false);

    vfx::RenderBackend::instance().setExternalDiagnosticStatus(
        QStringLiteral("Native WebGPU diagnostic is starting in an isolated helper process. "
                       "The tiled CPU reference path remains available while the GPU canvas backend initialises."));

    QObject::connect(process, &QProcess::readyRead, process, [process, output] {
        const QByteArray chunk = process->readAll();
        output->append(chunk);
        const QString text = QString::fromUtf8(chunk);
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty()
                && !trimmed.startsWith(
                    QString::fromLatin1(kGpuCapabilitiesMarker))) {
                qInfo().noquote() << trimmed;
            }
        }
    });

    QObject::connect(process,
                     &QProcess::errorOccurred,
                     process,
                     [process](const QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            const QString status = QStringLiteral(
                "The isolated native WebGPU diagnostic could not start: %1. "
                "The CPU renderer remains active.")
                                       .arg(process->errorString());
            vfx::RenderBackend::instance().setExternalDiagnosticStatus(status);
            qWarning().noquote() << "VFX Photo Lab render backend:" << status;
            process->deleteLater();
        }
    });

    QObject::connect(process,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     process,
                     [process, output, timedOut](const int exitCode,
                                                 const QProcess::ExitStatus exitStatus) {
        output->append(process->readAll());
        if (*timedOut) {
            process->deleteLater();
            return;
        }

        const QString allOutput = QString::fromUtf8(*output);
        QString resultStatus;
        bool diagnosticPassed = false;
        const QString resultMarker = QStringLiteral("VFXPHOTOLAB_GPU_RESULT=");
        const qsizetype resultMarkerIndex = allOutput.lastIndexOf(resultMarker);
        if (resultMarkerIndex >= 0) {
            const QString resultLine = allOutput.mid(resultMarkerIndex)
                                           .section(QLatin1Char('\n'), 0, 0);
            const QString resultCode = resultLine.section(QLatin1Char('\t'), 0, 0)
                                           .mid(resultMarker.size())
                                           .trimmed();
            resultStatus = resultLine.section(QLatin1Char('\t'), 1).trimmed();
            diagnosticPassed = resultCode == QStringLiteral("PASS")
                && exitStatus == QProcess::NormalExit
                && exitCode == 0;
        }

        vfx::GpuDiagnosticCapabilities capabilities;
        bool capabilitiesDecoded = false;
        const QString capabilitiesMarker =
            QString::fromLatin1(kGpuCapabilitiesMarker);
        const qsizetype capabilitiesIndex =
            allOutput.lastIndexOf(capabilitiesMarker);
        if (capabilitiesIndex >= 0) {
            const QString line = allOutput.mid(capabilitiesIndex)
                                     .section(QLatin1Char('\n'), 0, 0);
            capabilitiesDecoded = decodeGpuCapabilities(
                line.mid(capabilitiesMarker.size()).trimmed().toLatin1(),
                &capabilities);
        }

        if (resultStatus.isEmpty() || (diagnosticPassed && !capabilitiesDecoded)) {
            diagnosticPassed = false;
            resultStatus = QStringLiteral(
                "The isolated native WebGPU diagnostic ended without a complete "
                "validated capability record (exit code %1, process status %2). "
                "The CPU renderer remains active.")
                               .arg(exitCode)
                               .arg(exitStatus == QProcess::NormalExit
                                        ? QStringLiteral("normal exit")
                                        : QStringLiteral("crashed"));
        }

        if (capabilitiesDecoded) {
            if (!diagnosticPassed) {
                capabilities.foundation = false;
                capabilities.webGpu.foundation = false;
            }
            vfx::RenderBackend::instance().setExternalDiagnosticResult(
                resultStatus, capabilities);
        } else {
            vfx::RenderBackend::instance().setExternalDiagnosticStatus(
                resultStatus, false);
        }
        qInfo().noquote() << "VFX Photo Lab render backend:" << resultStatus;
        process->deleteLater();
    });

    process->start(QCoreApplication::applicationFilePath(),
                   QStringList{QString::fromLatin1(kGpuSelfTestArgument)});

    QTimer::singleShot(kGpuHelperTimeoutMs, process, [process, timedOut] {
        if (process->state() == QProcess::NotRunning) {
            return;
        }

        *timedOut = true;
        const QString status = QStringLiteral(
            "The isolated native WebGPU diagnostic exceeded 20 seconds and was stopped. "
            "The application remains usable with the CPU renderer; the terminal's last "
            "[GPU diagnostic] line identifies the stalled stage.");
        vfx::RenderBackend::instance().setExternalDiagnosticStatus(status);
        qWarning().noquote() << "VFX Photo Lab render backend:" << status;
        process->kill();
    });
}

} // namespace

int main(int argc, char *argv[])
{
    // Never allow a native graphics-driver probe to sit in front of the GUI
    // startup boundary. The normal process launches this mode as a disposable
    // helper and kills it if a driver call stalls.
    if (hasArgument(argc, argv, kPackageSmokeTestArgument)) {
        return runPackageSmokeTest(argc, argv);
    }
    if (hasArgument(argc, argv, kGpuSelfTestArgument)) {
        return runGpuSelfTest(argc, argv);
    }

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VFX Photo Lab"));
    QApplication::setApplicationVersion(QString::fromLatin1(VFXPHOTOLAB_VERSION));
    QApplication::setOrganizationName(QStringLiteral("VFX Suite"));
    const QString desktopId = QStringLiteral("io.github.MattyGWS.VFXPhotoLab");
    const bool packagedApplication = qEnvironmentVariableIsSet("FLATPAK_ID")
        || qEnvironmentVariableIsSet("APPIMAGE")
        || !QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
                                   desktopId + QStringLiteral(".desktop")).isEmpty();
    if (packagedApplication) {
        QApplication::setDesktopFileName(desktopId);
    }
    QApplication::setStyle(QStringLiteral("Fusion"));

    const QSettings appearanceSettings;
    const vfx::AppTheme startupTheme = vfx::applicationThemeFromId(
        appearanceSettings.value(QStringLiteral("appearance/theme"),
                                 vfx::applicationThemeId(vfx::defaultApplicationTheme()))
            .toString());
    vfx::setActiveApplicationTheme(startupTheme);
    application.setPalette(vfx::applicationPalette());
    application.setStyleSheet(vfx::applicationStyleSheet());

    // Photo editing legitimately exceeds Qt's conservative default image
    // allocation ceiling. Keep a finite guard against damaged files while
    // allowing multi-hundred-megapixel images on capable machines.
    QImageReader::setAllocationLimit(4096);

    QThreadPool::globalInstance()->setMaxThreadCount(
        std::max(2, QThread::idealThreadCount()));

    auto window = std::make_unique<vfx::MainWindow>();
    window->show();

    // Start only after the first window has been queued for presentation. The
    // diagnostic is a separate process, so even a stuck Vulkan/driver call can
    // neither block the GUI nor hold up application shutdown.
    QTimer::singleShot(0, &application, [&application] {
        startIsolatedGpuDiagnostic(application);
    });

    const QStringList arguments = application.arguments();
    QStringList initialPaths;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QString::fromLatin1(kGpuSelfTestArgument)
            || argument.startsWith(QLatin1Char('-'))) {
            continue;
        }
        initialPaths.push_back(QFileInfo(argument).absoluteFilePath());
    }
    if (!initialPaths.isEmpty()) {
        QTimer::singleShot(0, window.get(), [windowPointer = window.get(), initialPaths] {
            for (const QString &initialPath : initialPaths) {
                windowPointer->openFile(initialPath);
            }
        });
    }

    const int exitCode = application.exec();

    // Stop the disposable helper first, then let all document/render workers
    // finish while their owning widgets and the GPU backend still exist.
    // Destroying ImageCanvas and its QImage paint engines before releasing
    // wgpu-native objects prevents native teardown from corrupting memory that
    // Qt subsequently frees during QWidget destruction.
    const auto gpuHelpers = application.findChildren<QProcess *>(
        QStringLiteral("vfxphotolabGpuDiagnostic"));
    for (QProcess *helper : gpuHelpers) {
        if (helper && helper->state() != QProcess::NotRunning) {
            helper->kill();
            helper->waitForFinished(2000);
        }
    }
    QThreadPool::globalInstance()->waitForDone();
    window.reset();
    vfx::RenderBackend::instance().shutdownGpuFoundation();
    return exitCode;
}
