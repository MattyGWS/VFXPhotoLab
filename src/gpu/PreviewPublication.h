#pragma once

#include <cstdint>

namespace vfx {

// Settled content renders are accepted only when they exactly represent the
// latest requested document state.
constexpr bool previewPublicationIsExact(const std::uint64_t batchGeneration,
                                         const std::uint64_t batchRequestSerial,
                                         const std::uint64_t requestedRenderRevision,
                                         const std::uint64_t previewSnapshotGeneration,
                                         const std::uint64_t previewRequestSerial)
{
    return batchGeneration == requestedRenderRevision
        && batchGeneration == previewSnapshotGeneration
        && batchRequestSerial == previewRequestSerial;
}

// Adjustment drags deliberately permit the one currently executing frame to
// advance the display even when a newer slider value has already been queued.
// There is only one preview worker, so accepted interactive generations remain
// strictly ordered. This avoids starving presentation under high-rate pointer
// events while still rejecting a frame after the gesture has ended or after a
// different request has replaced the active viewport job.
constexpr bool previewPublicationMayAdvanceInteraction(
    const bool interactionActive,
    const bool interactiveBatch,
    const std::uint64_t batchGeneration,
    const std::uint64_t batchRequestSerial,
    const std::uint64_t previewSnapshotGeneration,
    const std::uint64_t stagedGeneration,
    const std::uint64_t stagedRequestSerial)
{
    return interactionActive
        && interactiveBatch
        && batchGeneration == previewSnapshotGeneration
        && batchGeneration == stagedGeneration
        && batchRequestSerial == stagedRequestSerial;
}

// A detail-sensitive spatial interaction already publishes authoritative
// level-0 pixels. Mouse release may adopt that generation directly only when
// it is the latest requested state and no worker/queued batch can still replace
// it. Blur interaction mips deliberately do not use this shortcut.
constexpr bool detailPreviewMaySettleWithoutRerender(
    const bool detailSensitiveInteraction,
    const std::uint64_t lastPublishedLevelZeroGeneration,
    const std::uint64_t requestedRenderRevision,
    const bool previewWorkerRunning,
    const bool previewQueueEmpty)
{
    return detailSensitiveInteraction
        && lastPublishedLevelZeroGeneration == requestedRenderRevision
        && !previewWorkerRunning
        && previewQueueEmpty;
}

} // namespace vfx
