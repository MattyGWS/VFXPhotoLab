#pragma once

#include "Adjustment.h"
#include "ColourManagement.h"

#include <QImage>
#include <QString>
#include <QVector>

#include <atomic>

namespace vfx {

class PhotoDocument;

enum class DocumentProfileOperation : quint8 {
    Assign,
    Convert
};

struct PreparedColourProfileResult {
    QImage canvasImage;
    QVector<LayerNode> layers;
    DocumentColourState colourState;
    DocumentProfileOperation operation = DocumentProfileOperation::Assign;
    int processedRasterImages = 0;
    int processedSemanticColours = 0;
    bool cancelled = false;
    QString error;

    bool isValid() const;
};

QVector<ColourSpaceDescriptor> commonWorkingColourSpaces();

bool loadExternalIccProfile(const QString &filePath,
                            ColourSpaceDescriptor *descriptor,
                            QString *errorMessage = nullptr);

// Apply one ordinary source-to-destination colour-space conversion to a
// straight RGBA image. Alpha and hidden RGB are preserved; monitor, proof and
// OCIO Display/View presentation transforms are deliberately outside this API.
bool transformImageColourSpace(
    QImage *image,
    const OcioConfigReference &ocioConfig,
    const ColourSpaceDescriptor &source,
    const ColourSpaceDescriptor &destination,
    ColourTransformPurpose purpose,
    ColourRenderingIntent renderingIntent = ColourRenderingIntent::RelativeColorimetric,
    bool blackPointCompensation = true,
    const std::atomic_bool *cancelRequested = nullptr,
    QString *errorMessage = nullptr);

bool prepareAssignedDocumentProfile(const PhotoDocument &document,
                                    const ColourSpaceDescriptor &target,
                                    PreparedColourProfileResult *result,
                                    QString *errorMessage = nullptr);

bool prepareConvertedDocumentProfile(const PhotoDocument &document,
                                      const ColourSpaceDescriptor &target,
                                      PreparedColourProfileResult *result,
                                      const std::atomic_bool *cancelRequested = nullptr,
                                      QString *errorMessage = nullptr);

} // namespace vfx
