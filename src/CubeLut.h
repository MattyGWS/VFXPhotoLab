#pragma once

#include "Adjustment.h"

#include <QByteArray>
#include <QColorSpace>
#include <QSize>
#include <QVector>
#include <QtCore/qfloat16.h>
#include <QString>

#include <array>

namespace vfx {


struct LutGpuTextureData {
    QSize size;
    QVector<qfloat16> rgba16f;
    int shaperRow = 0;

    bool isValid() const
    {
        return size.isValid() && !size.isEmpty()
            && rgba16f.size() == qsizetype(size.width()) * size.height() * 4;
    }
};

enum class LutDocumentTransfer {
    EncodedSrgb,
    LinearSrgb,
    UnsupportedProfile
};

class CubeLut final {
public:
    // Strict header-first Cube parser. Duplicate/conflicting declarations,
    // unsupported semantic directives and any directive after table data are
    // rejected with an actionable line-numbered error. Finite table values
    // are retained without display-range clipping.
    static bool parse(const QByteArray &contents,
                      const QString &sourceName,
                      LutParameters *parameters,
                      QString *error = nullptr);
    static bool loadFile(const QString &filePath,
                         LutParameters *parameters,
                         QString *error = nullptr);

    // Authoritative scalar evaluator: generic LUTs follow the persisted
    // processing contract, while named operator profiles add their required
    // scene-linear preprocessing and output operation. All paths apply an
    // optional 1D shaper before the 3D table, use deterministic interpolation
    // and blend Strength in document component space. Results remain unclamped
    // until the destination image format writes them.
    static std::array<double, 3> evaluate(
        const LutParameters &parameters,
        const std::array<double, 3> &input,
        LutDocumentTransfer documentTransfer = LutDocumentTransfer::EncodedSrgb);

    // Maps the document profile to the transfer-function state required by the
    // LUT contract. Full arbitrary ICC/OCIO conversion remains 0.11.0 work;
    // unsupported profiles deliberately receive no hidden gamut conversion.
    static LutDocumentTransfer documentTransferFor(const QColorSpace &colourSpace);
    static bool requiresCpuEvaluation(const LutParameters &parameters,
                                      const QColorSpace &colourSpace);
    // Returns an actionable, table-specific reason when the LUT cannot use
    // the native RGBA16Float/f32 transport. Runtime adapter/parity and 16-bit
    // document fallbacks are reported separately by the render backend/UI.
    static QString gpuFallbackReason(const LutParameters &parameters);
    static QString processingWarning(const LutParameters &parameters,
                                     const QColorSpace &colourSpace);

    // Packs the authoritative table payload directly into RGBA16Float texels.
    // A 3D LUT is laid out with red varying fastest: x = red + blue * size,
    // y = green. An optional 1D shaper occupies one final row. No QImage or
    // display-range quantisation is involved; extended finite values survive
    // whenever they are representable by IEEE-754 binary16.
    static LutGpuTextureData buildGpuTextureData(const LutParameters &parameters,
                                                 QString *error = nullptr);
};

} // namespace vfx
