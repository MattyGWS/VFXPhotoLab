#pragma once

#include "ColourManagement.h"

#include <QImage>
#include <QString>

namespace vfx {

// Inspect the decoded image and, where practical, the source container for an
// embedded ICC declaration. Valid profile bytes remain attached to the QImage;
// damaged/unsupported declarations never prevent the pixels from opening.
ImageColourImportInfo inspectImageColourProfile(const QString &filePath,
                                                QImage *decodedImage);

// Build equivalent metadata for a clipboard image. The caller identifies
// private VFX Photo Lab payloads separately from generic external clipboard
// images so the document can report their origin accurately.
ImageColourImportInfo inspectClipboardColourProfile(const QImage &image,
                                                    bool privateApplicationPayload);

// Resolve an untagged or unusable source profile without converting pixels.
// AssumeSRgb assigns an sRGB interpretation; LeaveUntagged keeps the QImage
// untagged. Ask must be resolved by the UI before this function is called.
void applyUntaggedImagePolicy(QImage *image,
                              ImageColourImportInfo *info,
                              UntaggedImagePolicy policy);

} // namespace vfx
