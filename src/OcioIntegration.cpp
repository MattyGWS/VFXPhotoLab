#include "OcioIntegration.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#ifdef VFXPHOTOLAB_HAS_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

namespace vfx {
namespace {

constexpr auto BuiltInCgUri = "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5";
constexpr auto BuiltInStudioUri = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) *errorMessage = message;
}

bool cancelled(const std::atomic_bool *token)
{
    return token && token->load(std::memory_order_acquire);
}

#ifdef VFXPHOTOLAB_HAS_OCIO

constexpr auto BuiltInSrgbTexture = "sRGB - Texture";

QString safeUtf8(const char *text)
{
    return text ? QString::fromUtf8(text) : QString();
}

QByteArray configFingerprint(const QString &identifier,
                             const QString &version,
                             const QByteArray &cacheId)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(identifier.toUtf8());
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(version.toUtf8());
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(cacheId);
    return hash.result();
}

QColorSpace srgbSpace()
{
    return QColorSpace(QColorSpace::SRgb);
}

bool isSrgbDescriptor(const ColourSpaceDescriptor &descriptor)
{
    return descriptor.kind == ColourSpaceKind::BuiltIn
        && descriptor.builtIn == BuiltInColourSpace::SRgb;
}

struct LoadedConfig {
    OCIO::ConstConfigRcPtr config;
    OcioConfigInspection inspection;
};

QMutex &configCacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QByteArray, std::shared_ptr<LoadedConfig>> &configCache()
{
    static QHash<QByteArray, std::shared_ptr<LoadedConfig>> cache;
    return cache;
}

QString canonicalExternalPath(const QString &path)
{
    QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool loadConfig(OcioConfigSource source,
                const QString &locator,
                std::shared_ptr<LoadedConfig> *loaded,
                QString *errorMessage)
{
    if (!loaded) {
        setError(errorMessage, QStringLiteral("The OCIO result destination is missing."));
        return false;
    }

    QString requested = locator.trimmed();
    if (source == OcioConfigSource::Environment) {
        requested = qEnvironmentVariable("OCIO").trimmed();
        if (requested.isEmpty()) {
            setError(errorMessage,
                     QStringLiteral("The OCIO environment variable is not set."));
            return false;
        }
    }
    if (source == OcioConfigSource::BuiltIn && requested.isEmpty()) {
        requested = QString::fromLatin1(BuiltInCgUri);
    }
    if (source == OcioConfigSource::ExternalFile) {
        requested = canonicalExternalPath(requested);
        const QFileInfo info(requested);
        if (!info.exists() || !info.isFile()) {
            setError(errorMessage,
                     QStringLiteral("The selected OCIO configuration file does not exist."));
            return false;
        }
    }
    if (requested.isEmpty()) {
        setError(errorMessage, QStringLiteral("No OCIO configuration was selected."));
        return false;
    }

    try {
        const QString resolved = requested;
        const QByteArray key = QCryptographicHash::hash(
            (QString::number(static_cast<int>(source)) + QLatin1Char('|') + resolved).toUtf8(),
            QCryptographicHash::Sha256);
        if (source == OcioConfigSource::BuiltIn) {
            QMutexLocker locker(&configCacheMutex());
            const auto found = configCache().constFind(key);
            if (found != configCache().cend()) {
                *loaded = found.value();
                return true;
            }
        }

        OCIO::ConstConfigRcPtr config = source == OcioConfigSource::BuiltIn
            ? OCIO::Config::CreateFromBuiltinConfig(resolved.toUtf8().constData())
            : OCIO::Config::CreateFromFile(resolved.toUtf8().constData());
        config->validate();

        auto result = std::make_shared<LoadedConfig>();
        result->config = config;
        OcioConfigInspection &inspection = result->inspection;
        inspection.available = true;
        inspection.reference.source = source;
        inspection.reference.identifier = resolved;
        inspection.reference.canonicalPath = source == OcioConfigSource::ExternalFile
            ? resolved : QString();
        inspection.reference.displayName = safeUtf8(config->getName()).trimmed();
        if (inspection.reference.displayName.isEmpty()) {
            inspection.reference.displayName = source == OcioConfigSource::BuiltIn
                ? (resolved.contains(QStringLiteral("studio-config"), Qt::CaseInsensitive)
                    ? QStringLiteral("ACES Studio Config 2.0")
                    : QStringLiteral("ACES CG Config 2.0"))
                : QFileInfo(resolved).fileName();
        }
        inspection.reference.version = QStringLiteral("%1.%2 / OCIO %3")
            .arg(config->getMajorVersion())
            .arg(config->getMinorVersion())
            .arg(QString::fromLatin1(OCIO::GetVersion()));
        inspection.reference.iccBridgeSpace = QString::fromLatin1(BuiltInSrgbTexture);
        inspection.reference.fingerprint = configFingerprint(
            resolved,
            inspection.reference.version,
            safeUtf8(config->getCacheID()).toUtf8());

        const int count = config->getNumColorSpaces();
        inspection.colourSpaces.reserve(count);
        for (int index = 0; index < count; ++index) {
            const char *name = config->getColorSpaceNameByIndex(index);
            if (!name || !*name) continue;
            const OCIO::ConstColorSpaceRcPtr colourSpace = config->getColorSpace(name);
            if (!colourSpace) continue;
            OcioColourSpaceInfo info;
            info.name = safeUtf8(name);
            info.family = safeUtf8(colourSpace->getFamily());
            info.description = safeUtf8(colourSpace->getDescription()).trimmed();
            info.encoding = safeUtf8(colourSpace->getEncoding());
            info.data = colourSpace->isData();
            info.sceneReferred = colourSpace->getReferenceSpaceType()
                == OCIO::REFERENCE_SPACE_SCENE;
            for (int categoryIndex = 0;
                 categoryIndex < colourSpace->getNumCategories();
                 ++categoryIndex) {
                info.categories.push_back(
                    safeUtf8(colourSpace->getCategory(categoryIndex)));
            }
            inspection.colourSpaces.push_back(std::move(info));
        }

        inspection.defaultDisplay = safeUtf8(config->getDefaultDisplay());
        for (int displayIndex = 0;
             displayIndex < config->getNumDisplays();
             ++displayIndex) {
            OcioDisplayInfo displayInfo;
            displayInfo.name = safeUtf8(config->getDisplay(displayIndex));
            displayInfo.defaultView = safeUtf8(
                config->getDefaultView(displayInfo.name.toUtf8().constData()));
            const int views = config->getNumViews(
                displayInfo.name.toUtf8().constData());
            for (int viewIndex = 0; viewIndex < views; ++viewIndex) {
                displayInfo.views.push_back(safeUtf8(config->getView(
                    displayInfo.name.toUtf8().constData(), viewIndex)));
            }
            inspection.displays.push_back(std::move(displayInfo));
        }
        inspection.defaultView = safeUtf8(config->getDefaultView(
            inspection.defaultDisplay.toUtf8().constData()));
        for (int lookIndex = 0; lookIndex < config->getNumLooks(); ++lookIndex) {
            inspection.looks.push_back(safeUtf8(config->getLookNameByIndex(lookIndex)));
        }

        const QStringList priorities = {
            QStringLiteral("ACEScg"),
            QStringLiteral("Linear Rec.709 (sRGB)"),
            QStringLiteral("ACES2065-1"),
            QStringLiteral("sRGB - Texture")
        };
        for (const QString &candidate : priorities) {
            const auto found = std::find_if(
                inspection.colourSpaces.cbegin(), inspection.colourSpaces.cend(),
                [&](const OcioColourSpaceInfo &space) {
                    return !space.data && space.sceneReferred
                        && space.name.compare(candidate, Qt::CaseInsensitive) == 0;
                });
            if (found != inspection.colourSpaces.cend()) {
                inspection.defaultWorkingSpace = found->name;
                break;
            }
        }
        if (inspection.defaultWorkingSpace.isEmpty()) {
            const auto found = std::find_if(
                inspection.colourSpaces.cbegin(), inspection.colourSpaces.cend(),
                [](const OcioColourSpaceInfo &space) {
                    return !space.data && space.sceneReferred;
                });
            if (found != inspection.colourSpaces.cend()) {
                inspection.defaultWorkingSpace = found->name;
            }
        }

        QString safetyError;
        if (!inspection.reference.isSafe(&safetyError)) {
            setError(errorMessage, safetyError);
            return false;
        }
        if (source == OcioConfigSource::BuiltIn) {
            QMutexLocker locker(&configCacheMutex());
            configCache().insert(key, result);
        }
        *loaded = std::move(result);
        return true;
    } catch (const OCIO::Exception &exception) {
        setError(errorMessage,
                 QStringLiteral("OpenColorIO could not load the configuration: %1")
                     .arg(QString::fromUtf8(exception.what())));
        return false;
    } catch (const std::exception &exception) {
        setError(errorMessage,
                 QStringLiteral("The OCIO configuration could not be loaded: %1")
                     .arg(QString::fromUtf8(exception.what())));
        return false;
    }
}

bool loadedForReference(const OcioConfigReference &reference,
                        std::shared_ptr<LoadedConfig> *loaded,
                        QString *errorMessage)
{
    const QString locator = reference.source == OcioConfigSource::ExternalFile
        ? reference.canonicalPath : reference.identifier;
    if (!loadConfig(reference.source, locator, loaded, errorMessage)) return false;
    return true;
}

#endif

} // namespace

struct OcioCpuTransform::Impl {
#ifdef VFXPHOTOLAB_HAS_OCIO
    OCIO::ConstCPUProcessorRcPtr ocioProcessor;
#endif
    std::optional<QColorTransform> preQt;
    std::optional<QColorTransform> postQt;
    QColorSpace targetQtSpace;
    QByteArray cacheId;
    bool valid = false;
};

struct OcioDisplayTransform::Impl {
#ifdef VFXPHOTOLAB_HAS_OCIO
    QVector<OCIO::ConstCPUProcessorRcPtr> processors;
#endif
    std::optional<QColorTransform> preQt;
    QByteArray cacheId;
    bool valid = false;
};

OcioDisplayTransform::OcioDisplayTransform(std::shared_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

OcioDisplayTransform::~OcioDisplayTransform() = default;

bool OcioDisplayTransform::isValid() const
{
    return m_impl && m_impl->valid;
}

QByteArray OcioDisplayTransform::cacheId() const
{
    return m_impl ? m_impl->cacheId : QByteArray();
}

OcioCpuTransform::OcioCpuTransform(std::shared_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

OcioCpuTransform::~OcioCpuTransform() = default;

bool OcioCpuTransform::isValid() const
{
    return m_impl && m_impl->valid;
}

QByteArray OcioCpuTransform::cacheId() const
{
    return m_impl ? m_impl->cacheId : QByteArray();
}

bool ocioSupportCompiled()
{
#ifdef VFXPHOTOLAB_HAS_OCIO
    return true;
#else
    return false;
#endif
}

QString ocioLibraryVersion()
{
#ifdef VFXPHOTOLAB_HAS_OCIO
    return QString::fromLatin1(OCIO::GetVersion());
#else
    return QStringLiteral("Not available in this build");
#endif
}

QVector<OcioBuiltInConfigChoice> ocioBuiltInConfigChoices()
{
    return {
        {QStringLiteral("ACES CG Config 2.0"),
         QString::fromLatin1(BuiltInCgUri),
         QStringLiteral("Compact ACES 2.0 configuration for image, texture and CG workflows.")},
        {QStringLiteral("ACES Studio Config 2.0"),
         QString::fromLatin1(BuiltInStudioUri),
         QStringLiteral("Expanded ACES 2.0 configuration with camera, cinema and post-production spaces.")}
    };
}

bool inspectOcioConfiguration(OcioConfigSource source,
                              const QString &locator,
                              OcioConfigInspection *inspection,
                              QString *errorMessage)
{
    if (!inspection) {
        setError(errorMessage, QStringLiteral("The OCIO inspection destination is missing."));
        return false;
    }
    *inspection = {};
#ifdef VFXPHOTOLAB_HAS_OCIO
    if (source == OcioConfigSource::None) {
        inspection->reference = OcioConfigReference::disabled();
        return true;
    }
    std::shared_ptr<LoadedConfig> loaded;
    if (!loadConfig(source, locator, &loaded, errorMessage)) return false;
    *inspection = loaded->inspection;
    return true;
#else
    Q_UNUSED(source)
    Q_UNUSED(locator)
    setError(errorMessage,
             QStringLiteral("This build does not contain OpenColorIO. Rebuild after installing or fetching OpenColorIO 2.5.2."));
    return false;
#endif
}

bool resolveOcioConfiguration(const OcioConfigReference &reference,
                              OcioConfigInspection *inspection,
                              QString *errorMessage)
{
    if (reference.source == OcioConfigSource::None) {
        if (inspection) {
            *inspection = {};
            inspection->reference = OcioConfigReference::disabled();
        }
        return true;
    }
    const QString locator = reference.source == OcioConfigSource::ExternalFile
        ? reference.canonicalPath : reference.identifier;
    if (!inspectOcioConfiguration(reference.source, locator,
                                  inspection, errorMessage)) {
        return false;
    }
    if (inspection) {
        inspection->fingerprintMatchesSavedReference =
            reference.fingerprint.isEmpty()
            || reference.fingerprint == inspection->reference.fingerprint;
        if (!inspection->fingerprintMatchesSavedReference) {
            inspection->warning = QStringLiteral(
                "The resolved OCIO configuration differs from the version saved with this document. No transform is substituted automatically.");
        }
    }
    return true;
}

QVector<ColourSpaceDescriptor> ocioWorkingColourSpaces(
    const OcioConfigInspection &inspection)
{
    QVector<ColourSpaceDescriptor> descriptors;
    if (!inspection.available || !inspection.reference.isConfigured()) return descriptors;

    const QStringList priorities = {
        QStringLiteral("ACEScg"),
        QStringLiteral("ACES2065-1"),
        QStringLiteral("Linear Rec.709 (sRGB)"),
        QStringLiteral("sRGB - Texture")
    };
    const auto appendByName = [&](const QString &name) {
        const auto found = std::find_if(
            inspection.colourSpaces.cbegin(), inspection.colourSpaces.cend(),
            [&](const OcioColourSpaceInfo &space) {
                return !space.data && space.sceneReferred
                    && space.name.compare(name, Qt::CaseInsensitive) == 0;
            });
        if (found == inspection.colourSpaces.cend()) return;
        const ColourSpaceDescriptor descriptor = ColourSpaceDescriptor::ocio(
            inspection.reference.identifier,
            inspection.reference.fingerprint,
            found->name,
            found->name);
        if (ocioQtWorkingSpaceProxy(descriptor).isValid()) {
            descriptors.push_back(descriptor);
        }
    };
    for (const QString &name : priorities) appendByName(name);

    // 0.11.0e intentionally exposes only spaces that have an exact Qt
    // matrix/TRC proxy. This keeps the 0.11.0d encoded/linear adjustment
    // contracts correct for an OCIO working document. Camera-log and other
    // complex encodings remain visible in the configuration inspector but are
    // not offered as authoritative integer document working spaces yet.
    return descriptors;
}

QVector<ColourSpaceDescriptor> ocioExportColourSpaces(
    const OcioConfigInspection &inspection)
{
    QVector<ColourSpaceDescriptor> descriptors;
    if (!inspection.available || !inspection.reference.isConfigured()) {
        return descriptors;
    }
    descriptors.reserve(inspection.colourSpaces.size());
    for (const OcioColourSpaceInfo &space : inspection.colourSpaces) {
        if (space.data || space.name.trimmed().isEmpty()) continue;
        descriptors.push_back(ColourSpaceDescriptor::ocio(
            inspection.reference.identifier,
            inspection.reference.fingerprint,
            space.name,
            space.name));
    }
    std::sort(descriptors.begin(), descriptors.end(),
              [](const ColourSpaceDescriptor &left,
                 const ColourSpaceDescriptor &right) {
                  return left.displayName.compare(
                      right.displayName, Qt::CaseInsensitive) < 0;
              });
    return descriptors;
}

QColorSpace ocioQtWorkingSpaceProxy(const ColourSpaceDescriptor &descriptor)
{
    if (descriptor.kind != ColourSpaceKind::Ocio) return {};
    const QString name = descriptor.ocioSpace.trimmed();
    if (name.compare(QStringLiteral("sRGB - Texture"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Utility - sRGB - Texture"), Qt::CaseInsensitive) == 0) {
        return QColorSpace(QColorSpace::SRgb);
    }
    if (name.compare(QStringLiteral("Linear Rec.709 (sRGB)"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Linear sRGB"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("Utility - Linear - sRGB"), Qt::CaseInsensitive) == 0) {
        return QColorSpace(QColorSpace::SRgbLinear);
    }

    const QPointF d60(0.32168, 0.33767);
    QColorSpace proxy;
    if (name.compare(QStringLiteral("ACEScg"), Qt::CaseInsensitive) == 0) {
        proxy = QColorSpace(
            d60,
            QPointF(0.713, 0.293),
            QPointF(0.165, 0.830),
            QPointF(0.128, 0.044),
            QColorSpace::TransferFunction::Linear);
        proxy.setDescription(QStringLiteral("ACEScg working proxy"));
    } else if (name.compare(QStringLiteral("ACES2065-1"), Qt::CaseInsensitive) == 0) {
        proxy = QColorSpace(
            d60,
            QPointF(0.7347, 0.2653),
            QPointF(0.0, 1.0),
            QPointF(0.0001, -0.077),
            QColorSpace::TransferFunction::Linear);
        proxy.setDescription(QStringLiteral("ACES2065-1 working proxy"));
    }
    if (proxy.isValid()) return proxy;

    // Qt 6.8 on Windows rejects ACES AP0/AP1 chromaticities because some of
    // their mathematically valid primaries lie just outside Qt's accepted xy
    // gamut. The QColorSpace attached here is deliberately only an integer-
    // image adjustment-domain proxy; OCIO remains authoritative for the actual
    // source/destination primaries. Fall back to a valid linear transfer proxy
    // so ACES integer documents can keep their explicit OCIO descriptor without
    // being rejected solely by Qt's metadata validator.
    if (name.compare(QStringLiteral("ACEScg"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("ACES2065-1"), Qt::CaseInsensitive) == 0) {
        QColorSpace linearProxy(QColorSpace::SRgbLinear);
        linearProxy.setDescription(name + QStringLiteral(" linear working proxy"));
        return linearProxy;
    }
    return {};
}

std::shared_ptr<const OcioCpuTransform> createOcioCpuTransform(
    const OcioConfigReference &configReference,
    const ColourSpaceDescriptor &source,
    const ColourSpaceDescriptor &destination,
    QString *errorMessage,
    const bool requireIntegerWorkingProxy)
{
#ifdef VFXPHOTOLAB_HAS_OCIO
    if (!configReference.isConfigured()) {
        setError(errorMessage, QStringLiteral("No OCIO configuration is active."));
        return {};
    }
    if (source.kind != ColourSpaceKind::Ocio
        && destination.kind != ColourSpaceKind::Ocio) {
        setError(errorMessage, QStringLiteral("The requested transform does not involve an OCIO colour space."));
        return {};
    }
    std::shared_ptr<LoadedConfig> loaded;
    if (!loadedForReference(configReference, &loaded, errorMessage)) return {};
    if (!configReference.fingerprint.isEmpty()
        && loaded->inspection.reference.fingerprint != configReference.fingerprint) {
        setError(errorMessage,
                 QStringLiteral("The OCIO configuration has changed since this document was saved. Relink or explicitly accept the current configuration before converting pixels."));
        return {};
    }

    const auto descriptorMatches = [&](const ColourSpaceDescriptor &descriptor) {
        return descriptor.kind != ColourSpaceKind::Ocio
            || (descriptor.ocioConfigId == configReference.identifier
                && descriptor.ocioConfigFingerprint == configReference.fingerprint);
    };
    if (!descriptorMatches(source) || !descriptorMatches(destination)) {
        setError(errorMessage,
                 QStringLiteral("The selected OCIO colour space belongs to a different configuration."));
        return {};
    }

    try {
        auto impl = std::make_shared<OcioCpuTransform::Impl>();
        OCIO::ConstProcessorRcPtr processor;
        if (source.kind == ColourSpaceKind::Ocio
            && destination.kind == ColourSpaceKind::Ocio) {
            processor = loaded->config->getProcessor(
                source.ocioSpace.toUtf8().constData(),
                destination.ocioSpace.toUtf8().constData());
        } else if (destination.kind == ColourSpaceKind::Ocio) {
            const ColourSpaceDescriptor bridge = ColourSpaceDescriptor::fromQColorSpace(srgbSpace());
            if (!isSrgbDescriptor(source)) {
                ColourTransformRequest request;
                request.source = source;
                request.destination = bridge;
                request.purpose = ColourTransformPurpose::WorkingToWorking;
                impl->preQt = ColourTransformService::instance().qtTransform(request);
                if (!impl->preQt) {
                    setError(errorMessage,
                             QStringLiteral("The source ICC profile could not be bridged to sRGB for OCIO conversion."));
                    return {};
                }
            }
            processor = OCIO::Config::GetProcessorFromBuiltinColorSpace(
                BuiltInSrgbTexture,
                loaded->config,
                destination.ocioSpace.toUtf8().constData());
        } else {
            processor = OCIO::Config::GetProcessorToBuiltinColorSpace(
                loaded->config,
                source.ocioSpace.toUtf8().constData(),
                BuiltInSrgbTexture);
            if (!isSrgbDescriptor(destination)) {
                const ColourSpaceDescriptor bridge = ColourSpaceDescriptor::fromQColorSpace(srgbSpace());
                ColourTransformRequest request;
                request.source = bridge;
                request.destination = destination;
                request.purpose = ColourTransformPurpose::WorkingToWorking;
                impl->postQt = ColourTransformService::instance().qtTransform(request);
                if (!impl->postQt) {
                    setError(errorMessage,
                             QStringLiteral("The OCIO result could not be bridged from sRGB into the destination ICC profile."));
                    return {};
                }
            }
            impl->targetQtSpace = destination.toQColorSpace();
        }
        if (destination.kind == ColourSpaceKind::Ocio) {
            impl->targetQtSpace = ocioQtWorkingSpaceProxy(destination);
            if (requireIntegerWorkingProxy && !impl->targetQtSpace.isValid()) {
                setError(errorMessage,
                         QStringLiteral("The selected OCIO destination cannot be represented safely as an integer document working space."));
                return {};
            }
        }
        impl->ocioProcessor = processor->getDefaultCPUProcessor();
        impl->cacheId = safeUtf8(impl->ocioProcessor->getCacheID()).toUtf8();
        impl->valid = static_cast<bool>(impl->ocioProcessor);
        return std::shared_ptr<const OcioCpuTransform>(
            new OcioCpuTransform(std::move(impl)));
    } catch (const OCIO::Exception &exception) {
        setError(errorMessage,
                 QStringLiteral("OpenColorIO could not create the colour transform: %1")
                     .arg(QString::fromUtf8(exception.what())));
        return {};
    }
#else
    Q_UNUSED(configReference)
    Q_UNUSED(source)
    Q_UNUSED(destination)
    Q_UNUSED(requireIntegerWorkingProxy)
    setError(errorMessage,
             QStringLiteral("This build does not contain OpenColorIO support."));
    return {};
#endif
}

QColor mapOcioSemanticColour(const QColor &colour,
                             const OcioCpuTransform &transform,
                             bool *ok)
{
    if (ok) *ok = false;
    if (!colour.isValid()) {
        if (ok) *ok = true;
        return colour;
    }
#ifdef VFXPHOTOLAB_HAS_OCIO
    if (!transform.m_impl || !transform.m_impl->valid
        || !transform.m_impl->ocioProcessor) {
        return {};
    }
    QColor working = colour;
    if (transform.m_impl->preQt) {
        working = transform.m_impl->preQt->map(working);
    }
    float rgba[4] = {
        static_cast<float>(working.redF()),
        static_cast<float>(working.greenF()),
        static_cast<float>(working.blueF()),
        static_cast<float>(colour.alphaF())
    };
    transform.m_impl->ocioProcessor->applyRGBA(rgba);
    QColor result = QColor::fromRgbF(
        std::clamp(static_cast<double>(rgba[0]), 0.0, 1.0),
        std::clamp(static_cast<double>(rgba[1]), 0.0, 1.0),
        std::clamp(static_cast<double>(rgba[2]), 0.0, 1.0),
        colour.alphaF());
    if (transform.m_impl->postQt) {
        result = transform.m_impl->postQt->map(result);
        result.setAlphaF(colour.alphaF());
    }
    if (ok) *ok = result.isValid();
    return result;
#else
    Q_UNUSED(transform)
    return {};
#endif
}

bool applyOcioCpuTransform(QImage *image,
                           const OcioCpuTransform &transform,
                           const std::atomic_bool *cancelRequested,
                           QString *errorMessage)
{
    if (!image || image->isNull()) return true;
    if (!transform.isValid()) {
        setError(errorMessage, QStringLiteral("The OCIO CPU transform is invalid."));
        return false;
    }
    if (cancelled(cancelRequested)) return false;
#ifdef VFXPHOTOLAB_HAS_OCIO
    if (!transform.m_impl || !transform.m_impl->ocioProcessor) {
        setError(errorMessage, QStringLiteral("The OCIO CPU processor is unavailable."));
        return false;
    }

    const QImage::Format originalFormat = image->format();
    const bool sixteenBit = originalFormat == QImage::Format_RGBA64
        || originalFormat == QImage::Format_RGBX64;
    QImage working = *image;
    if (sixteenBit) {
        if (working.format() != QImage::Format_RGBA64) {
            working = working.convertToFormat(QImage::Format_RGBA64);
        }
    } else if (working.format() != QImage::Format_RGBA8888) {
        working = working.convertToFormat(QImage::Format_RGBA8888);
    }
    if (working.isNull()) {
        setError(errorMessage, QStringLiteral("The raster image could not be prepared for OCIO processing."));
        return false;
    }

    QVector<float> rowData(static_cast<qsizetype>(working.width()) * 4);
    for (int y = 0; y < working.height(); ++y) {
        if (cancelled(cancelRequested)) return false;

        if (sixteenBit) {
            const auto *row = reinterpret_cast<const QRgba64 *>(working.constScanLine(y));
            for (int x = 0; x < working.width(); ++x) {
                QColor source = QColor::fromRgbF(
                    row[x].red() / 65535.0,
                    row[x].green() / 65535.0,
                    row[x].blue() / 65535.0,
                    row[x].alpha() / 65535.0);
                if (transform.m_impl->preQt) {
                    source = transform.m_impl->preQt->map(source);
                }
                const qsizetype offset = static_cast<qsizetype>(x) * 4;
                rowData[offset + 0] = static_cast<float>(source.redF());
                rowData[offset + 1] = static_cast<float>(source.greenF());
                rowData[offset + 2] = static_cast<float>(source.blueF());
                rowData[offset + 3] = static_cast<float>(row[x].alpha() / 65535.0);
            }
        } else {
            const uchar *row = working.constScanLine(y);
            for (int x = 0; x < working.width(); ++x) {
                const uchar *pixel = row + x * 4;
                QColor source = QColor::fromRgbF(
                    pixel[0] / 255.0,
                    pixel[1] / 255.0,
                    pixel[2] / 255.0,
                    pixel[3] / 255.0);
                if (transform.m_impl->preQt) {
                    source = transform.m_impl->preQt->map(source);
                }
                const qsizetype offset = static_cast<qsizetype>(x) * 4;
                rowData[offset + 0] = static_cast<float>(source.redF());
                rowData[offset + 1] = static_cast<float>(source.greenF());
                rowData[offset + 2] = static_cast<float>(source.blueF());
                rowData[offset + 3] = static_cast<float>(pixel[3] / 255.0);
            }
        }

        OCIO::PackedImageDesc rowDescription(
            rowData.data(), working.width(), 1, 4);
        transform.m_impl->ocioProcessor->apply(rowDescription);

        if (sixteenBit) {
            auto *row = reinterpret_cast<QRgba64 *>(working.scanLine(y));
            for (int x = 0; x < working.width(); ++x) {
                const QRgba64 original = row[x];
                const qsizetype offset = static_cast<qsizetype>(x) * 4;
                QColor mapped = QColor::fromRgbF(
                    std::clamp(static_cast<double>(rowData[offset + 0]), 0.0, 1.0),
                    std::clamp(static_cast<double>(rowData[offset + 1]), 0.0, 1.0),
                    std::clamp(static_cast<double>(rowData[offset + 2]), 0.0, 1.0),
                    original.alpha() / 65535.0);
                if (transform.m_impl->postQt) {
                    mapped = transform.m_impl->postQt->map(mapped);
                }
                row[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::clamp(
                        std::lround(mapped.redF() * 65535.0), 0L, 65535L)),
                    static_cast<quint16>(std::clamp(
                        std::lround(mapped.greenF() * 65535.0), 0L, 65535L)),
                    static_cast<quint16>(std::clamp(
                        std::lround(mapped.blueF() * 65535.0), 0L, 65535L)),
                    original.alpha());
            }
        } else {
            uchar *row = working.scanLine(y);
            for (int x = 0; x < working.width(); ++x) {
                uchar *pixel = row + x * 4;
                const uchar alpha = pixel[3];
                const qsizetype offset = static_cast<qsizetype>(x) * 4;
                QColor mapped = QColor::fromRgbF(
                    std::clamp(static_cast<double>(rowData[offset + 0]), 0.0, 1.0),
                    std::clamp(static_cast<double>(rowData[offset + 1]), 0.0, 1.0),
                    std::clamp(static_cast<double>(rowData[offset + 2]), 0.0, 1.0),
                    alpha / 255.0);
                if (transform.m_impl->postQt) {
                    mapped = transform.m_impl->postQt->map(mapped);
                }
                pixel[0] = static_cast<uchar>(std::clamp(
                    std::lround(mapped.redF() * 255.0), 0L, 255L));
                pixel[1] = static_cast<uchar>(std::clamp(
                    std::lround(mapped.greenF() * 255.0), 0L, 255L));
                pixel[2] = static_cast<uchar>(std::clamp(
                    std::lround(mapped.blueF() * 255.0), 0L, 255L));
                pixel[3] = alpha;
            }
        }
    }
    if (working.format() != originalFormat) {
        working = working.convertToFormat(originalFormat);
    }
    if (transform.m_impl->targetQtSpace.isValid()) {
        working.setColorSpace(transform.m_impl->targetQtSpace);
    } else {
        working.setColorSpace({});
    }
    *image = std::move(working);
    return true;
#else
    Q_UNUSED(transform)
    Q_UNUSED(cancelRequested)
    setError(errorMessage, QStringLiteral("This build does not contain OpenColorIO support."));
    return false;
#endif
}

std::shared_ptr<const OcioDisplayTransform> createOcioDisplayTransform(
    const OcioConfigReference &configReference,
    const ColourSpaceDescriptor &source,
    const QString &display,
    const QString &view,
    const QString &look,
    QString *errorMessage)
{
#ifdef VFXPHOTOLAB_HAS_OCIO
    std::shared_ptr<LoadedConfig> loaded;
    if (!loadedForReference(configReference, &loaded, errorMessage)) return {};
    if (!configReference.fingerprint.isEmpty()
        && configReference.fingerprint != loaded->inspection.reference.fingerprint) {
        setError(errorMessage,
                 QStringLiteral("The OCIO configuration fingerprint no longer matches the document."));
        return {};
    }
    if (display.trimmed().isEmpty() || view.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("The OCIO display and view must both be selected."));
        return {};
    }
    if (source.kind == ColourSpaceKind::Ocio
        && (source.ocioConfigId != configReference.identifier
            || source.ocioConfigFingerprint != configReference.fingerprint)) {
        setError(errorMessage,
                 QStringLiteral("The document working space belongs to a different OCIO configuration."));
        return {};
    }
    try {
        auto impl = std::make_shared<OcioDisplayTransform::Impl>();
        QString sourceSpace;
        if (source.kind == ColourSpaceKind::Ocio) {
            sourceSpace = source.ocioSpace;
        } else {
            sourceSpace = loaded->inspection.defaultWorkingSpace;
            const ColourSpaceDescriptor srgb = ColourSpaceDescriptor::fromQColorSpace(srgbSpace());
            if (!isSrgbDescriptor(source)) {
                ColourTransformRequest request;
                request.source = source;
                request.destination = srgb;
                request.purpose = ColourTransformPurpose::WorkingToDisplay;
                impl->preQt = ColourTransformService::instance().qtTransform(request);
                if (!impl->preQt) {
                    setError(errorMessage,
                             QStringLiteral("The ICC working profile could not be bridged to sRGB for OCIO display processing."));
                    return {};
                }
            }
            const OCIO::ConstProcessorRcPtr bridge =
                OCIO::Config::GetProcessorFromBuiltinColorSpace(
                    BuiltInSrgbTexture,
                    loaded->config,
                    sourceSpace.toUtf8().constData());
            if (!bridge) {
                setError(errorMessage,
                         QStringLiteral("OpenColorIO could not create the sRGB interchange processor."));
                return {};
            }
            impl->processors.push_back(bridge->getDefaultCPUProcessor());
        }

        if (!look.trimmed().isEmpty()) {
            if (!loaded->inspection.looks.contains(look)) {
                setError(errorMessage,
                         QStringLiteral("The selected OCIO look is not present in this configuration."));
                return {};
            }
            OCIO::LookTransformRcPtr lookTransform = OCIO::LookTransform::Create();
            lookTransform->setSrc(sourceSpace.toUtf8().constData());
            lookTransform->setDst(sourceSpace.toUtf8().constData());
            lookTransform->setLooks(look.toUtf8().constData());
            impl->processors.push_back(
                loaded->config->getProcessor(lookTransform)->getDefaultCPUProcessor());
        }

        OCIO::DisplayViewTransformRcPtr displayTransform =
            OCIO::DisplayViewTransform::Create();
        displayTransform->setSrc(sourceSpace.toUtf8().constData());
        displayTransform->setDisplay(display.toUtf8().constData());
        displayTransform->setView(view.toUtf8().constData());
        displayTransform->setLooksBypass(false);
        impl->processors.push_back(
            loaded->config->getProcessor(displayTransform)->getDefaultCPUProcessor());

        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(configReference.fingerprint);
        hash.addData(source.stableFingerprint());
        hash.addData(display.toUtf8());
        hash.addData(view.toUtf8());
        hash.addData(look.toUtf8());
        for (const auto &processor : std::as_const(impl->processors)) {
            if (processor) hash.addData(safeUtf8(processor->getCacheID()).toUtf8());
        }
        impl->cacheId = hash.result();
        impl->valid = !impl->processors.isEmpty()
            && std::all_of(impl->processors.cbegin(), impl->processors.cend(),
                           [](const auto &processor) { return static_cast<bool>(processor); });
        if (!impl->valid) {
            setError(errorMessage,
                     QStringLiteral("OpenColorIO returned an invalid Display/View CPU processor."));
            return {};
        }
        return std::shared_ptr<const OcioDisplayTransform>(
            new OcioDisplayTransform(std::move(impl)));
    } catch (const OCIO::Exception &exception) {
        setError(errorMessage,
                 QStringLiteral("OpenColorIO could not create the Display/View transform: %1")
                     .arg(QString::fromUtf8(exception.what())));
        return {};
    }
#else
    Q_UNUSED(configReference)
    Q_UNUSED(source)
    Q_UNUSED(display)
    Q_UNUSED(view)
    Q_UNUSED(look)
    setError(errorMessage, QStringLiteral("This build does not contain OpenColorIO support."));
    return {};
#endif
}

bool applyOcioDisplayTransform(QImage *image,
                               const OcioDisplayTransform &transform,
                               const std::atomic_bool *cancelRequested,
                               QString *errorMessage)
{
    if (!image || image->isNull()) return true;
    if (!transform.isValid()) {
        setError(errorMessage, QStringLiteral("The OCIO Display/View processor is invalid."));
        return false;
    }
    if (cancelled(cancelRequested)) return false;
#ifdef VFXPHOTOLAB_HAS_OCIO
    if (!transform.m_impl || transform.m_impl->processors.isEmpty()) {
        setError(errorMessage, QStringLiteral("The OCIO Display/View processor is unavailable."));
        return false;
    }
    const QImage::Format originalFormat = image->format();
    const bool sixteenBit = originalFormat == QImage::Format_RGBA64
        || originalFormat == QImage::Format_RGBX64;
    QImage working = sixteenBit
        ? image->convertToFormat(QImage::Format_RGBA64)
        : image->convertToFormat(QImage::Format_RGBA8888);
    if (working.isNull()) {
        setError(errorMessage, QStringLiteral("The image could not be prepared for OCIO display processing."));
        return false;
    }
    QVector<float> rowData(static_cast<qsizetype>(working.width()) * 4);
    for (int y = 0; y < working.height(); ++y) {
        if (cancelled(cancelRequested)) return false;
        if (sixteenBit) {
            const auto *row = reinterpret_cast<const QRgba64 *>(working.constScanLine(y));
            for (int x = 0; x < working.width(); ++x) {
                QColor colour = QColor::fromRgbF(row[x].red() / 65535.0,
                                                  row[x].green() / 65535.0,
                                                  row[x].blue() / 65535.0,
                                                  row[x].alpha() / 65535.0);
                if (transform.m_impl->preQt) colour = transform.m_impl->preQt->map(colour);
                const qsizetype o = static_cast<qsizetype>(x) * 4;
                rowData[o] = static_cast<float>(colour.redF());
                rowData[o + 1] = static_cast<float>(colour.greenF());
                rowData[o + 2] = static_cast<float>(colour.blueF());
                rowData[o + 3] = static_cast<float>(row[x].alpha() / 65535.0);
            }
        } else {
            const uchar *row = working.constScanLine(y);
            for (int x = 0; x < working.width(); ++x) {
                const uchar *pixel = row + x * 4;
                QColor colour = QColor::fromRgbF(pixel[0] / 255.0,
                                                  pixel[1] / 255.0,
                                                  pixel[2] / 255.0,
                                                  pixel[3] / 255.0);
                if (transform.m_impl->preQt) colour = transform.m_impl->preQt->map(colour);
                const qsizetype o = static_cast<qsizetype>(x) * 4;
                rowData[o] = static_cast<float>(colour.redF());
                rowData[o + 1] = static_cast<float>(colour.greenF());
                rowData[o + 2] = static_cast<float>(colour.blueF());
                rowData[o + 3] = static_cast<float>(pixel[3] / 255.0);
            }
        }
        OCIO::PackedImageDesc rowDescription(rowData.data(), working.width(), 1, 4);
        for (const auto &processor : std::as_const(transform.m_impl->processors)) {
            processor->apply(rowDescription);
        }
        if (sixteenBit) {
            auto *row = reinterpret_cast<QRgba64 *>(working.scanLine(y));
            for (int x = 0; x < working.width(); ++x) {
                const quint16 alpha = row[x].alpha();
                const qsizetype o = static_cast<qsizetype>(x) * 4;
                row[x] = QRgba64::fromRgba64(
                    static_cast<quint16>(std::clamp(std::lround(rowData[o] * 65535.0f), 0L, 65535L)),
                    static_cast<quint16>(std::clamp(std::lround(rowData[o + 1] * 65535.0f), 0L, 65535L)),
                    static_cast<quint16>(std::clamp(std::lround(rowData[o + 2] * 65535.0f), 0L, 65535L)),
                    alpha);
            }
        } else {
            uchar *row = working.scanLine(y);
            for (int x = 0; x < working.width(); ++x) {
                uchar *pixel = row + x * 4;
                const uchar alpha = pixel[3];
                const qsizetype o = static_cast<qsizetype>(x) * 4;
                pixel[0] = static_cast<uchar>(std::clamp(std::lround(rowData[o] * 255.0f), 0L, 255L));
                pixel[1] = static_cast<uchar>(std::clamp(std::lround(rowData[o + 1] * 255.0f), 0L, 255L));
                pixel[2] = static_cast<uchar>(std::clamp(std::lround(rowData[o + 2] * 255.0f), 0L, 255L));
                pixel[3] = alpha;
            }
        }
    }
    if (working.format() != originalFormat) working = working.convertToFormat(originalFormat);
    working.setColorSpace(QColorSpace(QColorSpace::SRgb));
    *image = std::move(working);
    return true;
#else
    Q_UNUSED(transform)
    Q_UNUSED(cancelRequested)
    setError(errorMessage, QStringLiteral("This build does not contain OpenColorIO support."));
    return false;
#endif
}

bool validateOcioDisplayView(const OcioConfigReference &configReference,
                             const QString &sourceSpace,
                             const QString &display,
                             const QString &view,
                             const QString &look,
                             QString *errorMessage)
{
#ifdef VFXPHOTOLAB_HAS_OCIO
    std::shared_ptr<LoadedConfig> loaded;
    if (!loadedForReference(configReference, &loaded, errorMessage)) return false;
    if (!configReference.fingerprint.isEmpty()
        && configReference.fingerprint != loaded->inspection.reference.fingerprint) {
        setError(errorMessage, QStringLiteral("The OCIO configuration fingerprint no longer matches the document."));
        return false;
    }
    try {
        if (!look.trimmed().isEmpty()) {
            if (!loaded->inspection.looks.contains(look)) {
                setError(errorMessage,
                         QStringLiteral("The selected OCIO look is not present in this configuration."));
                return false;
            }
            OCIO::LookTransformRcPtr lookTransform = OCIO::LookTransform::Create();
            lookTransform->setSrc(sourceSpace.toUtf8().constData());
            lookTransform->setDst(sourceSpace.toUtf8().constData());
            lookTransform->setLooks(look.toUtf8().constData());
            const OCIO::ConstProcessorRcPtr lookProcessor =
                loaded->config->getProcessor(lookTransform);
            if (!lookProcessor || !lookProcessor->getDefaultCPUProcessor()) {
                setError(errorMessage,
                         QStringLiteral("The selected OCIO look did not produce a CPU processor."));
                return false;
            }
        }
        OCIO::DisplayViewTransformRcPtr transform = OCIO::DisplayViewTransform::Create();
        transform->setSrc(sourceSpace.toUtf8().constData());
        transform->setDisplay(display.toUtf8().constData());
        transform->setView(view.toUtf8().constData());
        transform->setLooksBypass(false);
        const OCIO::ConstProcessorRcPtr displayProcessor =
            loaded->config->getProcessor(transform);
        if (!displayProcessor || !displayProcessor->getDefaultCPUProcessor()) {
            setError(errorMessage,
                     QStringLiteral("The selected OCIO display/view did not produce a CPU processor."));
            return false;
        }
        return true;
    } catch (const OCIO::Exception &exception) {
        setError(errorMessage,
                 QStringLiteral("The OCIO display/view selection is invalid: %1")
                     .arg(QString::fromUtf8(exception.what())));
        return false;
    }
#else
    Q_UNUSED(configReference)
    Q_UNUSED(sourceSpace)
    Q_UNUSED(display)
    Q_UNUSED(view)
    Q_UNUSED(look)
    setError(errorMessage, QStringLiteral("This build does not contain OpenColorIO support."));
    return false;
#endif
}

} // namespace vfx
