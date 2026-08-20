#include "TgaCodec.h"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <limits>

namespace vfx {
namespace {

quint16 readLe16(const uchar *data)
{
    return qFromLittleEndian<quint16>(data);
}

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool checkedPixelCount(const int width, const int height, qsizetype *count)
{
    if (width <= 0 || height <= 0) {
        return false;
    }

    const qint64 product = static_cast<qint64>(width) * static_cast<qint64>(height);
    if (product <= 0 || product > std::numeric_limits<int>::max()) {
        return false;
    }

    *count = static_cast<qsizetype>(product);
    return true;
}

} // namespace

QImage TgaCodec::read(const QString &filePath, QString *errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorMessage, file.errorString());
        return {};
    }

    const QByteArray bytes = file.readAll();
    if (bytes.size() < 18) {
        setError(errorMessage, QStringLiteral("The TGA file is too small to contain a valid header."));
        return {};
    }

    const auto *raw = reinterpret_cast<const uchar *>(bytes.constData());
    const int idLength = raw[0];
    const int colourMapType = raw[1];
    const int imageType = raw[2];
    const int width = readLe16(raw + 12);
    const int height = readLe16(raw + 14);
    const int pixelDepth = raw[16];
    const int descriptor = raw[17];

    const bool trueColour = imageType == 2 || imageType == 10;
    const bool greyscale = imageType == 3 || imageType == 11;
    const bool rle = imageType == 10 || imageType == 11;

    if (colourMapType != 0 || (!trueColour && !greyscale)) {
        setError(errorMessage,
                 QStringLiteral("Only unpaletted true-colour and greyscale TGA images are supported."));
        return {};
    }

    if ((trueColour && pixelDepth != 24 && pixelDepth != 32)
        || (greyscale && pixelDepth != 8)) {
        setError(errorMessage,
                 QStringLiteral("Only 24/32-bit colour and 8-bit greyscale TGA images are supported."));
        return {};
    }

    qsizetype pixelCount = 0;
    if (!checkedPixelCount(width, height, &pixelCount)) {
        setError(errorMessage, QStringLiteral("The TGA dimensions are invalid or too large."));
        return {};
    }

    qsizetype offset = 18 + idLength;
    if (offset > bytes.size()) {
        setError(errorMessage, QStringLiteral("The TGA image ID extends past the end of the file."));
        return {};
    }

    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull()) {
        setError(errorMessage, QStringLiteral("Could not allocate memory for the TGA image."));
        return {};
    }

    const bool topOrigin = (descriptor & 0x20) != 0;
    const bool rightOrigin = (descriptor & 0x10) != 0;
    const int bytesPerPixel = pixelDepth / 8;

    auto decodePixel = [&](const uchar *source, uchar *destination) {
        if (greyscale) {
            destination[0] = source[0];
            destination[1] = source[0];
            destination[2] = source[0];
            destination[3] = 255;
            return;
        }

        destination[0] = source[2];
        destination[1] = source[1];
        destination[2] = source[0];
        destination[3] = bytesPerPixel == 4 ? source[3] : 255;
    };

    qsizetype outputIndex = 0;
    auto writeDecodedPixel = [&](const uchar *source) {
        const int fileX = static_cast<int>(outputIndex % width);
        const int fileY = static_cast<int>(outputIndex / width);
        const int x = rightOrigin ? width - 1 - fileX : fileX;
        const int y = topOrigin ? fileY : height - 1 - fileY;
        uchar *destination = image.scanLine(y) + x * 4;
        decodePixel(source, destination);
        ++outputIndex;
    };

    if (!rle) {
        const qint64 required = static_cast<qint64>(pixelCount) * bytesPerPixel;
        if (required < 0 || offset + required > bytes.size()) {
            setError(errorMessage, QStringLiteral("The TGA pixel data is truncated."));
            return {};
        }

        while (outputIndex < pixelCount) {
            writeDecodedPixel(raw + offset);
            offset += bytesPerPixel;
        }
    } else {
        std::array<uchar, 4> pixel {};
        while (outputIndex < pixelCount) {
            if (offset >= bytes.size()) {
                setError(errorMessage, QStringLiteral("The TGA RLE stream is truncated."));
                return {};
            }

            const uchar packetHeader = raw[offset++];
            const int runLength = (packetHeader & 0x7f) + 1;
            if (outputIndex + runLength > pixelCount) {
                setError(errorMessage, QStringLiteral("The TGA RLE packet exceeds the image bounds."));
                return {};
            }

            if (packetHeader & 0x80) {
                if (offset + bytesPerPixel > bytes.size()) {
                    setError(errorMessage, QStringLiteral("The TGA RLE pixel is truncated."));
                    return {};
                }
                std::copy_n(raw + offset, bytesPerPixel, pixel.begin());
                offset += bytesPerPixel;
                for (int i = 0; i < runLength; ++i) {
                    writeDecodedPixel(pixel.data());
                }
            } else {
                const qint64 required = static_cast<qint64>(runLength) * bytesPerPixel;
                if (offset + required > bytes.size()) {
                    setError(errorMessage, QStringLiteral("The TGA raw packet is truncated."));
                    return {};
                }
                for (int i = 0; i < runLength; ++i) {
                    writeDecodedPixel(raw + offset);
                    offset += bytesPerPixel;
                }
            }
        }
    }

    return image;
}

bool TgaCodec::write(const QString &filePath,
                     const QImage &image,
                     QString *errorMessage)
{
    if (image.isNull()) {
        setError(errorMessage, QStringLiteral("There is no image to save."));
        return false;
    }

    if (image.width() > std::numeric_limits<quint16>::max()
        || image.height() > std::numeric_limits<quint16>::max()) {
        setError(errorMessage,
                 QStringLiteral("Classic TGA files cannot exceed 65,535 pixels in either dimension."));
        return false;
    }

    const QImage converted = image.convertToFormat(QImage::Format_RGBA8888);
    if (converted.isNull()) {
        setError(errorMessage, QStringLiteral("Could not convert the image for TGA export."));
        return false;
    }

    QSaveFile file(filePath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorMessage, file.errorString());
        return false;
    }

    QByteArray header(18, '\0');
    auto *headerData = reinterpret_cast<uchar *>(header.data());
    headerData[2] = 2; // Uncompressed true-colour image.
    qToLittleEndian<quint16>(static_cast<quint16>(converted.width()), headerData + 12);
    qToLittleEndian<quint16>(static_cast<quint16>(converted.height()), headerData + 14);
    headerData[16] = 32;
    headerData[17] = 0x28; // 8 alpha bits, top-left origin.

    if (file.write(header) != header.size()) {
        setError(errorMessage, file.errorString());
        return false;
    }

    QByteArray row(converted.width() * 4, '\0');
    for (int y = 0; y < converted.height(); ++y) {
        const uchar *source = converted.constScanLine(y);
        uchar *destination = reinterpret_cast<uchar *>(row.data());
        for (int x = 0; x < converted.width(); ++x) {
            destination[x * 4 + 0] = source[x * 4 + 2];
            destination[x * 4 + 1] = source[x * 4 + 1];
            destination[x * 4 + 2] = source[x * 4 + 0];
            destination[x * 4 + 3] = source[x * 4 + 3];
        }

        if (file.write(row) != row.size()) {
            setError(errorMessage, file.errorString());
            return false;
        }
    }

    if (!file.commit()) {
        setError(errorMessage, file.errorString());
        return false;
    }

    return true;
}

} // namespace vfx
