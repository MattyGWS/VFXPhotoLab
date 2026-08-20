#pragma once

#include "ColourManagement.h"

#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <atomic>

namespace vfx {

enum class ImageExportBitDepth : quint8 {
    Eight = 8,
    Sixteen = 16
};

enum class ImageExportDither : quint8 {
    None,
    BlueNoise64
};

enum class ImageExportAlphaMode : quint8 {
    PreserveWhenSupported,
    FlattenToMatte
};

struct ImageExportCapabilities {
    QString suffix;
    QString displayName;
    bool valid = false;
    bool supportsAlpha = false;
    bool supportsEightBit = true;
    bool supportsSixteenBit = false;
    bool supportsIccProfile = false;
    bool supportsQuality = false;
};

struct ImageExportRequest {
    QString filePath;
    int quality = 95;
    ImageExportBitDepth bitDepth = ImageExportBitDepth::Eight;
    ImageExportDither dither = ImageExportDither::BlueNoise64;
    ImageExportAlphaMode alphaMode = ImageExportAlphaMode::PreserveWhenSupported;
    bool convertToOutputProfile = true;
    OutputColourSettings output;
    QColor matteColour = Qt::white;
    quint32 ditherSeed = 0x01100008u;
};

struct PreparedImageExport {
    QImage image;
    ImageExportCapabilities capabilities;
    ColourSpaceDescriptor sourceProfile;
    ColourSpaceDescriptor outputProfile;
    ImageExportBitDepth bitDepth = ImageExportBitDepth::Eight;
    bool convertedToOutputProfile = false;
    bool profileEmbedded = false;
    bool dithered = false;
    bool flattenedTransparency = false;
    QStringList warnings;

    bool isValid() const;
};

ImageExportCapabilities imageExportCapabilitiesForPath(const QString &filePath);
bool imageExportWriterAvailable(const ImageExportCapabilities &capabilities);
QString imageExportBitDepthName(ImageExportBitDepth bitDepth);
QString imageExportDitherName(ImageExportDither dither);
QString imageExportAlphaModeName(ImageExportAlphaMode mode);

bool validateImageExportRequest(const ImageExportRequest &request,
                                const DocumentColourState &colourState,
                                QString *errorMessage = nullptr);

bool prepareImageExport(const QImage &renderedWorkingImage,
                        const DocumentColourState &colourState,
                        const ImageExportRequest &request,
                        PreparedImageExport *prepared,
                        const std::atomic_bool *cancelRequested = nullptr,
                        QString *errorMessage = nullptr);

bool writePreparedImageExport(const QString &filePath,
                              const PreparedImageExport &prepared,
                              int quality,
                              QString *errorMessage = nullptr);

// Public deterministic quantisation helper used by the export path and tests.
QImage quantizeExportToEightBit(
    const QImage &rgba64,
    ImageExportDither dither,
    quint32 seed = 0x01100008u,
    const std::atomic_bool *cancelRequested = nullptr);

} // namespace vfx
