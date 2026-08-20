#pragma once

#include "Adjustment.h"

#include <QByteArray>
#include <QColorSpace>
#include <QString>
#include <QVector>
#include <QtCore/qfloat16.h>

#include <memory>

namespace vfx {

// Reference-baked working-space <-> adjustment-domain transforms used only by
// the 8-bit tiled compositor. The exact CPU QColorTransform path remains the
// authority and rejects lattices that exceed the deterministic parity limit.
struct ManagedAdjustmentGpuLutData {
    int edgeSize = 0;
    AdjustmentProcessingDomain domain =
        AdjustmentProcessingDomain::EncodedWorking;
    QVector<qfloat16> workingToDomainRgba16f;
    QVector<qfloat16> domainToWorkingRgba16f;
    QByteArray fingerprint;
    int referenceMaximumDifference = -1;

    qsizetype texelCount() const
    {
        return edgeSize > 1
            ? qsizetype(edgeSize) * edgeSize * edgeSize
            : 0;
    }

    bool isValid() const
    {
        const qsizetype components = texelCount() * 4;
        return edgeSize > 1
            && workingToDomainRgba16f.size() == components
            && domainToWorkingRgba16f.size() == components
            && !fingerprint.isEmpty();
    }
};

std::shared_ptr<const ManagedAdjustmentGpuLutData>
createManagedAdjustmentGpuLut(const QColorSpace &workingSpace,
                              AdjustmentProcessingDomain domain,
                              QString *errorMessage = nullptr);

} // namespace vfx
