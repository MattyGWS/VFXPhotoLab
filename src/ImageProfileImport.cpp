#include "ImageProfileImport.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QByteArrayView>
#include <QIODevice>

#include <algorithm>
#include <array>

namespace vfx {
namespace {

constexpr qint64 MaximumInspectionBytes = 64LL * 1024LL * 1024LL;
constexpr qsizetype MaximumProfileBytes = 16 * 1024 * 1024;

struct ContainerProfileInspection {
    bool advertised = false;
    QByteArray profileBytes;
    QByteArray advertisedFingerprint;
};

quint16 readU16(const uchar *bytes, const bool littleEndian)
{
    return littleEndian
        ? static_cast<quint16>(bytes[0] | (quint16(bytes[1]) << 8))
        : static_cast<quint16>((quint16(bytes[0]) << 8) | bytes[1]);
}

quint32 readU32(const uchar *bytes, const bool littleEndian)
{
    if (littleEndian) {
        return quint32(bytes[0]) | (quint32(bytes[1]) << 8)
            | (quint32(bytes[2]) << 16) | (quint32(bytes[3]) << 24);
    }
    return (quint32(bytes[0]) << 24) | (quint32(bytes[1]) << 16)
        | (quint32(bytes[2]) << 8) | quint32(bytes[3]);
}

QByteArray sha256(const QByteArray &bytes)
{
    return bytes.isEmpty() ? QByteArray()
                           : QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

ContainerProfileInspection inspectPng(const QByteArray &bytes)
{
    static constexpr std::array<uchar, 8> signature {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
    ContainerProfileInspection result;
    if (bytes.size() < 12
        || !std::equal(signature.cbegin(), signature.cend(),
                       reinterpret_cast<const uchar *>(bytes.constData()))) {
        return result;
    }

    qsizetype offset = 8;
    while (offset + 12 <= bytes.size()) {
        const auto *chunk = reinterpret_cast<const uchar *>(bytes.constData() + offset);
        const quint32 length = readU32(chunk, false);
        if (length > static_cast<quint32>(MaximumInspectionBytes)
            || offset + 12 + static_cast<qsizetype>(length) > bytes.size()) {
            break;
        }
        const QByteArray type(bytes.constData() + offset + 4, 4);
        const QByteArray data(bytes.constData() + offset + 8,
                              static_cast<qsizetype>(length));
        if (type == QByteArrayLiteral("iCCP")) {
            result.advertised = true;
            result.advertisedFingerprint = sha256(data);
            return result;
        }
        if (type == QByteArrayLiteral("sRGB")) {
            result.advertised = true;
            result.advertisedFingerprint = sha256(QByteArrayLiteral("PNG/sRGB"));
            return result;
        }
        if (type == QByteArrayLiteral("IEND") || type == QByteArrayLiteral("IDAT")) {
            break;
        }
        offset += 12 + static_cast<qsizetype>(length);
    }
    return result;
}

ContainerProfileInspection inspectJpeg(const QByteArray &bytes)
{
    ContainerProfileInspection result;
    if (bytes.size() < 4
        || static_cast<uchar>(bytes[0]) != 0xff
        || static_cast<uchar>(bytes[1]) != 0xd8) {
        return result;
    }

    QHash<int, QByteArray> pieces;
    QByteArray advertisedMaterial;
    int expectedCount = 0;
    qsizetype offset = 2;
    while (offset + 4 <= bytes.size()) {
        if (static_cast<uchar>(bytes[offset]) != 0xff) {
            ++offset;
            continue;
        }
        while (offset < bytes.size()
               && static_cast<uchar>(bytes[offset]) == 0xff) {
            ++offset;
        }
        if (offset >= bytes.size()) break;
        const uchar marker = static_cast<uchar>(bytes[offset++]);
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (offset + 2 > bytes.size()) break;
        const quint16 segmentLength = readU16(
            reinterpret_cast<const uchar *>(bytes.constData() + offset), false);
        if (segmentLength < 2 || offset + segmentLength > bytes.size()) break;
        const qsizetype payloadOffset = offset + 2;
        const qsizetype payloadLength = segmentLength - 2;
        static constexpr char IccSignature[] = "ICC_PROFILE\0";
        constexpr qsizetype SignatureBytes = sizeof(IccSignature) - 1;
        if (marker == 0xe2 && payloadLength >= SignatureBytes + 2
            && QByteArrayView(bytes.constData() + payloadOffset, SignatureBytes)
                == QByteArrayView(IccSignature, SignatureBytes)) {
            result.advertised = true;
            if (advertisedMaterial.size() + payloadLength <= MaximumProfileBytes) {
                advertisedMaterial += bytes.mid(payloadOffset, payloadLength);
            }
            const int sequence = static_cast<uchar>(
                bytes[payloadOffset + SignatureBytes]);
            const int count = static_cast<uchar>(
                bytes[payloadOffset + SignatureBytes + 1]);
            if (sequence > 0 && count > 0 && sequence <= count
                && (expectedCount == 0 || expectedCount == count)) {
                expectedCount = count;
                pieces.insert(sequence,
                    bytes.mid(payloadOffset + SignatureBytes + 2,
                              payloadLength - SignatureBytes - 2));
            }
        }
        offset += segmentLength;
    }

    if (result.advertised && !advertisedMaterial.isEmpty()) {
        result.advertisedFingerprint = sha256(advertisedMaterial);
    }
    if (result.advertised && expectedCount > 0 && pieces.size() == expectedCount) {
        QByteArray profile;
        for (int sequence = 1; sequence <= expectedCount; ++sequence) {
            if (!pieces.contains(sequence)
                || profile.size() + pieces.value(sequence).size() > MaximumProfileBytes) {
                profile.clear();
                break;
            }
            profile += pieces.value(sequence);
        }
        result.profileBytes = profile;
        result.advertisedFingerprint = sha256(profile);
    }
    return result;
}

ContainerProfileInspection inspectTiff(const QByteArray &bytes)
{
    ContainerProfileInspection result;
    if (bytes.size() < 8) return result;
    const bool littleEndian = bytes.startsWith("II");
    if (!littleEndian && !bytes.startsWith("MM")) return result;
    const auto *raw = reinterpret_cast<const uchar *>(bytes.constData());
    if (readU16(raw + 2, littleEndian) != 42) return result;

    quint32 ifdOffset = readU32(raw + 4, littleEndian);
    for (int directory = 0; directory < 8 && ifdOffset != 0; ++directory) {
        if (ifdOffset + 2 > static_cast<quint32>(bytes.size())) break;
        const quint16 entries = readU16(raw + ifdOffset, littleEndian);
        const quint64 tableEnd = quint64(ifdOffset) + 2ULL + quint64(entries) * 12ULL + 4ULL;
        if (tableEnd > static_cast<quint64>(bytes.size())) break;
        for (quint16 index = 0; index < entries; ++index) {
            const quint32 entryOffset = ifdOffset + 2 + quint32(index) * 12;
            const quint16 tag = readU16(raw + entryOffset, littleEndian);
            if (tag != 34675) continue; // ICCProfile
            result.advertised = true;
            result.advertisedFingerprint = sha256(
                bytes.mid(entryOffset, 12));
            const quint16 type = readU16(raw + entryOffset + 2, littleEndian);
            if (type != 1 && type != 7) return result; // BYTE or UNDEFINED
            const quint32 count = readU32(raw + entryOffset + 4, littleEndian);
            if (count == 0 || count > static_cast<quint32>(MaximumProfileBytes)) {
                return result;
            }
            QByteArray profile;
            if (count <= 4) {
                profile = bytes.mid(entryOffset + 8, count);
            } else {
                const quint32 profileOffset = readU32(raw + entryOffset + 8, littleEndian);
                if (quint64(profileOffset) + count <= static_cast<quint64>(bytes.size())) {
                    profile = bytes.mid(profileOffset, count);
                }
            }
            result.profileBytes = profile;
            if (!profile.isEmpty()) {
                result.advertisedFingerprint = sha256(profile);
            }
            return result;
        }
        ifdOffset = readU32(raw + ifdOffset + 2 + quint32(entries) * 12,
                            littleEndian);
    }
    return result;
}

ContainerProfileInspection inspectContainer(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0
        || file.size() > MaximumInspectionBytes) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() >= 8
        && static_cast<uchar>(bytes[0]) == 0x89
        && bytes.mid(1, 3) == QByteArrayLiteral("PNG")) {
        return inspectPng(bytes);
    }
    if (bytes.size() >= 2
        && static_cast<uchar>(bytes[0]) == 0xff
        && static_cast<uchar>(bytes[1]) == 0xd8) {
        return inspectJpeg(bytes);
    }
    if (bytes.startsWith("II") || bytes.startsWith("MM")) {
        return inspectTiff(bytes);
    }
    return {};
}

} // namespace

ImageColourImportInfo inspectImageColourProfile(const QString &filePath,
                                                QImage *decodedImage)
{
    ImageColourImportInfo info;
    info.sourceStatus = InputProfileStatus::Untagged;
    if (!decodedImage || decodedImage->isNull()) {
        return info;
    }

    const ContainerProfileInspection container = inspectContainer(filePath);
    info.embeddedProfileAdvertised = container.advertised;
    info.originalProfileFingerprint = container.advertisedFingerprint;

    if (!decodedImage->colorSpace().isValid() && !container.profileBytes.isEmpty()) {
        const QColorSpace recovered = QColorSpace::fromIccProfile(container.profileBytes);
        if (recovered.isValid()) {
            decodedImage->setColorSpace(recovered);
        }
    }

    if (decodedImage->colorSpace().isValid()) {
        const QColorSpace containerSpace = QColorSpace::fromIccProfile(
            container.profileBytes);
        const QByteArray profileBytes = containerSpace.isValid()
            ? container.profileBytes
            : decodedImage->colorSpace().iccProfile();
        if (profileBytes.size() > MaximumProfileBytes) {
            info.sourceStatus = InputProfileStatus::InvalidOrUnsupported;
            info.embeddedProfileAdvertised = true;
            info.warnings.push_back(QStringLiteral(
                "The embedded colour profile exceeds the 16 MiB safety limit. "
                "The pixel values were preserved and the untagged-image policy was used instead."));
            decodedImage->setColorSpace(QColorSpace());
            return info;
        }
        info.sourceStatus = InputProfileStatus::EmbeddedValid;
        info.embeddedProfileAdvertised = true;
        info.originalIccProfile = profileBytes;
        info.originalProfileFingerprint = !info.originalIccProfile.isEmpty()
            ? sha256(info.originalIccProfile)
            : colourProfileContentFingerprint(decodedImage->colorSpace());
        return info;
    }

    if (container.advertised) {
        info.sourceStatus = InputProfileStatus::InvalidOrUnsupported;
        info.warnings.push_back(QStringLiteral(
            "The image declares an embedded colour profile, but it is damaged or unsupported. "
            "The pixel values were preserved and the untagged-image policy was used instead."));
    }
    return info;
}

ImageColourImportInfo inspectClipboardColourProfile(
    const QImage &image,
    const bool privateApplicationPayload)
{
    ImageColourImportInfo info;
    if (image.colorSpace().isValid()) {
        info.originalIccProfile = image.colorSpace().iccProfile();
        if (info.originalIccProfile.size() > MaximumProfileBytes) {
            info.sourceStatus = InputProfileStatus::InvalidOrUnsupported;
            info.embeddedProfileAdvertised = true;
            info.originalIccProfile.clear();
            info.warnings.push_back(QStringLiteral(
                "The clipboard colour profile exceeds the 16 MiB safety limit. "
                "The pixel values were preserved and the untagged-image policy was used instead."));
            return info;
        }
        info.sourceStatus = InputProfileStatus::ClipboardValid;
        info.embeddedProfileAdvertised = !privateApplicationPayload;
        info.originalProfileFingerprint = !info.originalIccProfile.isEmpty()
            ? sha256(info.originalIccProfile)
            : colourProfileContentFingerprint(image.colorSpace());
    } else {
        info.sourceStatus = InputProfileStatus::Untagged;
    }
    return info;
}

void applyUntaggedImagePolicy(QImage *image,
                              ImageColourImportInfo *info,
                              UntaggedImagePolicy policy)
{
    if (!image || image->isNull() || !info || !info->requiresUntaggedPolicy()) {
        return;
    }
    if (policy == UntaggedImagePolicy::Ask) {
        return;
    }
    info->appliedPolicy = policy;
    info->policyWasApplied = true;
    if (policy == UntaggedImagePolicy::AssumeSRgb) {
        image->setColorSpace(QColorSpace(QColorSpace::SRgb));
    } else {
        image->setColorSpace(QColorSpace());
    }
}

} // namespace vfx
