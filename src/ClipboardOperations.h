#pragma once

#include "SelectionMask.h"
#include "TransformInterpolation.h"

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTransform>

#include <optional>

namespace vfx {

enum class ClipboardImageKind : quint8 {
    Rgba = 0,
    Grayscale = 1
};

enum class ClipboardSourceKind : quint8 {
    RasterPixels = 0,
    Composite = 1,
    GreyChannel = 2,
    RedChannel = 3,
    GreenChannel = 4,
    BlueChannel = 5,
    AlphaChannel = 6,
    Mask = 7,
    ExternalImage = 8
};

enum class ClipboardPasteTarget : quint8 {
    Mask = 0,
    GreyChannel = 1,
    RedChannel = 2,
    GreenChannel = 3,
    BlueChannel = 4,
    AlphaChannel = 5
};

struct ClipboardPayload {
    static constexpr quint32 Magic = 0x56465843u; // VFXC
    static constexpr quint16 FormatVersion = 1;
    static constexpr const char *MimeType = "application/x-vfxphotolab-clipboard-v1";

    ClipboardImageKind imageKind = ClipboardImageKind::Rgba;
    ClipboardSourceKind sourceKind = ClipboardSourceKind::ExternalImage;
    QImage image;
    // For greyscale payloads this stores application coverage independently
    // from the copied values. RGBA payloads use image alpha directly.
    QImage coverage;
    QRect documentBounds;
    QSize sourceDocumentSize;
    bool hasDocumentPlacement = false;
    QString sourceName;

    bool isValid() const;
    QByteArray toBytes(bool *ok = nullptr) const;
    static std::optional<ClipboardPayload> fromBytes(const QByteArray &bytes,
                                                     QString *errorMessage = nullptr);
    QImage interoperabilityImage() const;
};

// Extract one layer-local raster, channel or mask into an axis-aligned,
// document-space clipboard payload. Selection coverage is baked into RGBA
// alpha or retained separately for greyscale values. localExtent describes the
// coordinate extent represented by sourceImage; a compact 1x1 mask may still
// represent a full document-sized local extent.
ClipboardPayload extractClipboardPayload(const QImage &sourceImage,
                                         const QSize &localExtent,
                                         const QTransform &localToDocument,
                                         const QSize &documentSize,
                                         const SelectionMask::Snapshot &selection,
                                         ClipboardSourceKind sourceKind,
                                         int channelIndex = -1,
                                         const QString &sourceName = {});

// Convert a payload into a straight RGBA raster suitable for an immediately
// committed pasted layer. Greyscale values become neutral RGB and their
// independent coverage becomes alpha.
QImage clipboardPayloadAsRaster(const ClipboardPayload &payload,
                                const QColorSpace &targetColourSpace,
                                bool targetSixteenBit,
                                bool targetGrayscale = false);

// Materialise a clipboard payload as the exact pixel raster for a new
// document. The payload's embedded colour space and integer precision are
// retained where available. The compatibility overload assigns sRGB to an
// untagged payload; the explicit overload is used by the colour-import policy
// after the UI has resolved whether to assume sRGB or leave components untagged.
// Document safety limits are enforced before allocating or opening a session.
QImage clipboardPayloadAsNewDocumentRaster(const ClipboardPayload &payload,
                                           QString *errorMessage = nullptr);
QImage clipboardPayloadAsNewDocumentRaster(const ClipboardPayload &payload,
                                           const QColorSpace &effectiveColourSpace,
                                           QString *errorMessage = nullptr);

// Materialise a cropped clipboard payload into the editor's full-document
// raster convention. This avoids the compositor interpreting a cropped image
// as a full-document raster and scaling it to the canvas.
QImage clipboardPayloadAsDocumentRaster(const ClipboardPayload &payload,
                                        const QColorSpace &targetColourSpace,
                                        bool targetSixteenBit,
                                        bool targetGrayscale,
                                        const QSize &documentSize);

struct ClipboardPasteResult {
    QImage image;
    QRect affectedRect;
    // True when inputs, inverse transforms and all required allocations were
    // valid, even when the transformed fragment lies wholly outside the target
    // or produces no changed samples. This lets transform commits distinguish a
    // safe empty result from an allocation/invertibility failure.
    bool succeeded = false;
    bool changed = false;
};

// Paste a clipboard payload into a direct channel or mask. Clipboard alpha (or
// greyscale coverage) is application coverage, multiplied by the destination
// document selection when one is active. The target stays in its original
// local coordinate system and bit depth.
ClipboardPasteResult pasteClipboardIntoTarget(
    const ClipboardPayload &payload,
    const QImage &targetImage,
    const QSize &targetExtent,
    const QTransform &targetToDocument,
    const QSize &documentSize,
    ClipboardPasteTarget target,
    const SelectionMask::Snapshot &destinationSelection,
    bool preferSixteenBit = false,
    const QTransform &payloadDocumentTransform = QTransform(),
    TransformInterpolation interpolation = TransformInterpolation::Bilinear);

// Merge an RGBA clipboard fragment into a raster target through an arbitrary
// document-space affine or projective transform. The target remains straight RGBA and hidden
// RGB is retained when the resulting alpha is zero.
ClipboardPasteResult pasteClipboardIntoRasterTarget(
    const ClipboardPayload &payload,
    const QImage &targetImage,
    const QSize &targetExtent,
    const QTransform &targetToDocument,
    const QSize &documentSize,
    bool preferSixteenBit = false,
    const QTransform &payloadDocumentTransform = QTransform(),
    TransformInterpolation interpolation = TransformInterpolation::Bilinear);

} // namespace vfx
