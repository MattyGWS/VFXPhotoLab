#include "gpu/WebGpuContext.h"

#include "AdjustmentShaderSource.h"
#include "DisplayColourShaderSource.h"
#include "VectorFeatherShaderSource.h"

#include "CubeLut.h"
#include "ImageProcessor.h"
#include "TonalMapping.h"
#include "VectorRasterizer.h"

#include <QByteArray>
#include <QColor>
#include <QCryptographicHash>
#include <QDebug>
#include <QHash>
#include <QList>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

#ifdef VFXPHOTOLAB_HAS_WEBGPU
#  if __has_include(<webgpu/wgpu.h>)
#    include <webgpu/wgpu.h>
#    define VFXPHOTOLAB_HAS_WGPU_NATIVE_POLL 1
#  elif __has_include(<webgpu/webgpu.h>)
#    include <webgpu/webgpu.h>
#  elif __has_include(<webgpu-headers/webgpu.h>)
#    include <webgpu-headers/webgpu.h>
#  else
#    error "VFXPHOTOLAB_HAS_WEBGPU is set but no WebGPU C header is available"
#  endif
#endif

namespace vfx {
namespace {

constexpr auto kCallbackTimeout = std::chrono::seconds(5);

#ifdef VFXPHOTOLAB_HAS_WEBGPU

WGPUStringView stringView(const char *text)
{
    WGPUStringView view = WGPU_STRING_VIEW_INIT;
    view.data = text;
    view.length = text ? std::strlen(text) : 0;
    return view;
}

QString fromStringView(const WGPUStringView view)
{
    if (!view.data || view.length == 0) {
        return {};
    }
    if (view.length == WGPU_STRLEN) {
        return QString::fromUtf8(view.data);
    }
    return QString::fromUtf8(view.data, static_cast<qsizetype>(view.length));
}

template<typename Handle, typename Status>
struct RequestState {
    std::mutex mutex;
    std::condition_variable condition;
    Handle handle = nullptr;
    Status status {};
    QString message;
    bool complete = false;
    bool abandoned = false;
};

struct MapState {
    std::mutex mutex;
    std::condition_variable condition;
    WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
    QString message;
    bool complete = false;
    bool abandoned = false;
};

struct UncapturedErrorState {
    std::mutex mutex;
    WGPUErrorType type = WGPUErrorType_NoError;
    QString message;
    quint64 serial = 0;
};

void onUncapturedError(WGPUDevice const *,
                       const WGPUErrorType type,
                       const WGPUStringView message,
                       void *userdata1,
                       void *)
{
    auto *state = static_cast<UncapturedErrorState *>(userdata1);
    const QString text = fromStringView(message);
    if (state) {
        std::lock_guard lock(state->mutex);
        state->type = type;
        state->message = text;
        ++state->serial;
    }

    // Keep application-owned diagnostics for uncaptured WebGPU errors. Some
    // invalid queue submissions remain fatal inside wgpu-native, so this
    // callback improves reporting but is not a substitute for correct resource
    // usage and pre-submit validation.
    qWarning().noquote()
        << "[GPU diagnostic] WebGPU validation error:"
        << (text.isEmpty() ? QStringLiteral("unknown WebGPU validation error") : text);
}

bool asciiIdentifierStart(const char value)
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || value == '_';
}

bool asciiIdentifierContinue(const char value)
{
    return asciiIdentifierStart(value) || (value >= '0' && value <= '9');
}

QString firstReservedWgslWord(const char *source)
{
    if (!source) {
        return {};
    }

    static constexpr std::string_view reservedWords[] = {
        "NULL", "Self", "abstract", "active", "alignas", "alignof", "as",
        "asm", "asm_fragment", "async", "attribute", "auto", "await",
        "become", "cast", "catch", "class", "co_await", "co_return",
        "co_yield", "coherent", "column_major", "common", "compile",
        "compile_fragment", "concept", "const_cast", "consteval", "constexpr",
        "constinit", "crate", "debugger", "decltype", "delete", "demote",
        "demote_to_helper", "do", "dynamic_cast", "enum", "explicit", "export",
        "extends", "extern", "external", "fallthrough", "filter", "final",
        "finally", "friend", "from", "fxgroup", "get", "goto", "groupshared",
        "highp", "impl", "implements", "import", "inline", "instanceof",
        "interface", "layout", "lowp", "macro", "macro_rules", "match",
        "mediump", "meta", "mod", "module", "move", "mut", "mutable",
        "namespace", "new", "nil", "noexcept", "noinline", "nointerpolation",
        "non_coherent", "noncoherent", "noperspective", "null", "nullptr", "of",
        "operator", "package", "packoffset", "partition", "pass", "patch",
        "pixelfragment", "precise", "precision", "premerge", "priv", "protected",
        "pub", "public", "readonly", "ref", "regardless", "register",
        "reinterpret_cast", "require", "resource", "restrict", "self", "set",
        "shared", "sizeof", "smooth", "snorm", "static", "static_assert",
        "static_cast", "std", "subroutine", "super", "target", "template", "this",
        "thread_local", "throw", "trait", "try", "type", "typedef", "typeid",
        "typename", "typeof", "union", "unless", "unorm", "unsafe", "unsized",
        "use", "using", "varying", "virtual", "volatile", "wgsl", "where",
        "with", "writeonly", "yield"
    };

    const std::string_view text(source);
    bool lineComment = false;
    size_t blockCommentDepth = 0;
    for (size_t index = 0; index < text.size();) {
        if (lineComment) {
            if (text[index] == '\n' || text[index] == '\r') {
                lineComment = false;
            }
            ++index;
            continue;
        }
        if (blockCommentDepth > 0) {
            if (index + 1 < text.size() && text[index] == '/' && text[index + 1] == '*') {
                ++blockCommentDepth;
                index += 2;
            } else if (index + 1 < text.size() && text[index] == '*' && text[index + 1] == '/') {
                --blockCommentDepth;
                index += 2;
            } else {
                ++index;
            }
            continue;
        }
        if (index + 1 < text.size() && text[index] == '/' && text[index + 1] == '/') {
            lineComment = true;
            index += 2;
            continue;
        }
        if (index + 1 < text.size() && text[index] == '/' && text[index + 1] == '*') {
            blockCommentDepth = 1;
            index += 2;
            continue;
        }
        if (!asciiIdentifierStart(text[index])) {
            ++index;
            continue;
        }

        const size_t begin = index++;
        while (index < text.size() && asciiIdentifierContinue(text[index])) {
            ++index;
        }
        const std::string_view token = text.substr(begin, index - begin);
        for (const std::string_view reserved : reservedWords) {
            if (token == reserved) {
                return QString::fromLatin1(token.data(), static_cast<qsizetype>(token.size()));
            }
        }
    }
    return {};
}

QString firstUnsupportedWgslSwizzleAssignment(const char *source)
{
    if (!source) {
        return {};
    }

    const std::string_view text(source);
    bool lineComment = false;
    size_t blockCommentDepth = 0;
    for (size_t index = 0; index < text.size();) {
        if (lineComment) {
            if (text[index] == '\n' || text[index] == '\r') {
                lineComment = false;
            }
            ++index;
            continue;
        }
        if (blockCommentDepth > 0) {
            if (index + 1 < text.size() && text[index] == '/' && text[index + 1] == '*') {
                ++blockCommentDepth;
                index += 2;
            } else if (index + 1 < text.size() && text[index] == '*' && text[index + 1] == '/') {
                --blockCommentDepth;
                index += 2;
            } else {
                ++index;
            }
            continue;
        }
        if (index + 1 < text.size() && text[index] == '/' && text[index + 1] == '/') {
            lineComment = true;
            index += 2;
            continue;
        }
        if (index + 1 < text.size() && text[index] == '/' && text[index + 1] == '*') {
            blockCommentDepth = 1;
            index += 2;
            continue;
        }
        if (text[index] != '.') {
            ++index;
            continue;
        }

        const size_t begin = ++index;
        while (index < text.size() && asciiIdentifierContinue(text[index])) {
            ++index;
        }
        const std::string_view accessor = text.substr(begin, index - begin);
        if (accessor.size() < 2 || accessor.size() > 4) {
            continue;
        }
        const bool rgba = std::all_of(accessor.begin(), accessor.end(), [](const char value) {
            return value == 'r' || value == 'g' || value == 'b' || value == 'a';
        });
        const bool xyzw = std::all_of(accessor.begin(), accessor.end(), [](const char value) {
            return value == 'x' || value == 'y' || value == 'z' || value == 'w';
        });
        if (!rgba && !xyzw) {
            continue;
        }
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t'
                                       || text[index] == '\n' || text[index] == '\r')) {
            ++index;
        }
        if (index < text.size() && text[index] == '='
            && (index + 1 >= text.size() || text[index + 1] != '=')) {
            return QStringLiteral(".%1")
                .arg(QString::fromLatin1(accessor.data(), static_cast<qsizetype>(accessor.size())));
        }
    }
    return {};
}

void onAdapterRequested(const WGPURequestAdapterStatus status,
                        const WGPUAdapter adapter,
                        const WGPUStringView message,
                        void *userdata1,
                        void *)
{
    auto *state = static_cast<RequestState<WGPUAdapter, WGPURequestAdapterStatus> *>(userdata1);
    bool abandoned = false;
    {
        std::lock_guard lock(state->mutex);
        state->status = status;
        state->handle = adapter;
        state->message = fromStringView(message);
        state->complete = true;
        abandoned = state->abandoned;
    }
    state->condition.notify_one();
    if (abandoned) {
        if (adapter) {
            wgpuAdapterRelease(adapter);
        }
        delete state;
    }
}

void onDeviceRequested(const WGPURequestDeviceStatus status,
                       const WGPUDevice device,
                       const WGPUStringView message,
                       void *userdata1,
                       void *)
{
    auto *state = static_cast<RequestState<WGPUDevice, WGPURequestDeviceStatus> *>(userdata1);
    bool abandoned = false;
    {
        std::lock_guard lock(state->mutex);
        state->status = status;
        state->handle = device;
        state->message = fromStringView(message);
        state->complete = true;
        abandoned = state->abandoned;
    }
    state->condition.notify_one();
    if (abandoned) {
        if (device) {
            wgpuDeviceRelease(device);
        }
        delete state;
    }
}

void onBufferMapped(const WGPUMapAsyncStatus status,
                    const WGPUStringView message,
                    void *userdata1,
                    void *)
{
    auto *state = static_cast<MapState *>(userdata1);
    bool abandoned = false;
    {
        std::lock_guard lock(state->mutex);
        state->status = status;
        state->message = fromStringView(message);
        state->complete = true;
        abandoned = state->abandoned;
    }
    state->condition.notify_one();
    if (abandoned) {
        delete state;
    }
}

uint32_t alignedBytesPerRow(const uint32_t width, const uint32_t bytesPerPixel = 4)
{
    constexpr uint32_t alignment = 256;
    const uint32_t unaligned = width * bytesPerPixel;
    return (unaligned + alignment - 1) & ~(alignment - 1);
}

QByteArray paddedImageBytes(const QImage &image, const uint32_t paddedRowBytes)
{
    const uint32_t width = static_cast<uint32_t>(image.width());
    const uint32_t height = static_cast<uint32_t>(image.height());
    QByteArray bytes(static_cast<qsizetype>(static_cast<uint64_t>(paddedRowBytes) * height), '\0');
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(bytes.data() + static_cast<qsizetype>(y * paddedRowBytes),
                    image.constScanLine(static_cast<int>(y)),
                    static_cast<size_t>(width) * 4);
    }
    return bytes;
}

WGPUTexture uploadTexture(WGPUDevice device,
                          WGPUQueue queue,
                          const QImage &rgba,
                          const char *label,
                          const bool writable,
                          QString *error)
{
    const uint32_t width = static_cast<uint32_t>(rgba.width());
    const uint32_t height = static_cast<uint32_t>(rgba.height());
    WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = stringView(label);
    // Uploaded textures are normally sampled, but the hierarchy compositor may
    // promote an opaque bottom raster directly to the current accumulator and
    // ultimately read it back without an intermediate compute pass. Keep every
    // uploaded RGBA texture copy-source capable so that optimisation remains a
    // valid WebGPU usage combination even for a one-layer document.
    descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc
        | WGPUTextureUsage_TextureBinding;
    if (writable) {
        descriptor.usage |= WGPUTextureUsage_StorageBinding;
    }
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = {width, height, 1};
    descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    WGPUTexture texture = wgpuDeviceCreateTexture(device, &descriptor);
    if (!texture) {
        if (error) *error = QStringLiteral("GPU texture allocation failed");
        return nullptr;
    }

    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const QByteArray upload = paddedImageBytes(rgba, paddedRowBytes);
    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = texture;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = paddedRowBytes;
    layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuQueueWriteTexture(queue,
                          &destination,
                          upload.constData(),
                          static_cast<size_t>(upload.size()),
                          &layout,
                          &extent);
    return texture;
}

WGPUTexture uploadLutTexture(WGPUDevice device,
                             WGPUQueue queue,
                             const LutGpuTextureData &lookup,
                             const char *label,
                             QString *error)
{
    static_assert(sizeof(qfloat16) == 2,
                  "RGBA16Float upload requires 16-bit qfloat16 storage");
    if (!lookup.isValid()) {
        if (error) *error = QStringLiteral("GPU LUT payload is empty or malformed");
        return nullptr;
    }
    const uint32_t width = static_cast<uint32_t>(lookup.size.width());
    const uint32_t height = static_cast<uint32_t>(lookup.size.height());
    WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = stringView(label);
    descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = {width, height, 1};
    descriptor.format = WGPUTextureFormat_RGBA16Float;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    WGPUTexture texture = wgpuDeviceCreateTexture(device, &descriptor);
    if (!texture) {
        if (error) *error = QStringLiteral("GPU RGBA16Float LUT allocation failed");
        return nullptr;
    }

    constexpr uint32_t BytesPerPixel = 4u * sizeof(qfloat16);
    const uint32_t sourceRowBytes = width * BytesPerPixel;
    const uint32_t paddedRowBytes = alignedBytesPerRow(width, BytesPerPixel);
    QByteArray upload(static_cast<qsizetype>(
                          static_cast<uint64_t>(paddedRowBytes) * height), '\0');
    const auto *source = reinterpret_cast<const char *>(lookup.rgba16f.constData());
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(upload.data() + static_cast<qsizetype>(y * paddedRowBytes),
                    source + static_cast<qsizetype>(y * sourceRowBytes),
                    sourceRowBytes);
    }

    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = texture;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = paddedRowBytes;
    layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuQueueWriteTexture(queue,
                          &destination,
                          upload.constData(),
                          static_cast<size_t>(upload.size()),
                          &layout,
                          &extent);
    return texture;
}

WGPUTexture createWorkingTexture(WGPUDevice device,
                                 const QSize &size,
                                 const char *label,
                                 QString *error)
{
    WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.label = stringView(label);
    descriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc
        | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding;
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = {static_cast<uint32_t>(size.width()),
                       static_cast<uint32_t>(size.height()), 1};
    descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    WGPUTexture texture = wgpuDeviceCreateTexture(device, &descriptor);
    if (!texture && error) {
        *error = QStringLiteral("GPU working texture allocation failed");
    }
    return texture;
}

QImage mapReadbackBuffer(WGPUDevice device,
                         WGPUBuffer buffer,
                         const QSize &size,
                         const uint32_t paddedRowBytes,
                         QString *error)
{
    const uint64_t bufferSize = static_cast<uint64_t>(paddedRowBytes)
        * static_cast<uint32_t>(size.height());
    auto mapState = std::make_unique<MapState>();
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = onBufferMapped;
    callback.userdata1 = mapState.get();
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, bufferSize, callback);
#ifdef VFXPHOTOLAB_HAS_WGPU_NATIVE_POLL
    wgpuDevicePoll(device, true, nullptr);
#endif
    {
        std::unique_lock lock(mapState->mutex);
        if (!mapState->condition.wait_for(lock, kCallbackTimeout, [&] { return mapState->complete; })) {
            mapState->abandoned = true;
            mapState.release();
            if (error) *error = QStringLiteral("Timed out waiting for GPU tile readback");
            return {};
        }
    }
    if (mapState->status != WGPUMapAsyncStatus_Success) {
        if (error) {
            *error = QStringLiteral("GPU tile readback failed: %1")
                         .arg(mapState->message.isEmpty() ? QStringLiteral("unknown map error")
                                                        : mapState->message);
        }
        return {};
    }
    const void *mapped = wgpuBufferGetConstMappedRange(buffer, 0, bufferSize);
    if (!mapped) {
        if (error) *error = QStringLiteral("GPU readback returned no mapped data");
        wgpuBufferUnmap(buffer);
        return {};
    }
    // rgba8unorm readbacks always contain four bytes per pixel. Keep the
    // host image fixed to the matching format so a future scalar caller cannot
    // accidentally copy RGBA rows into a one-byte QImage and corrupt the heap.
    QImage result(size, QImage::Format_RGBA8888);
    if (result.isNull() || result.bytesPerLine() < size.width() * 4) {
        if (error) *error = QStringLiteral("GPU readback image allocation failed");
        wgpuBufferUnmap(buffer);
        return {};
    }
    for (int y = 0; y < size.height(); ++y) {
        std::memcpy(result.scanLine(y),
                    static_cast<const char *>(mapped) + static_cast<size_t>(y) * paddedRowBytes,
                    static_cast<size_t>(size.width()) * 4);
    }
    wgpuBufferUnmap(buffer);
    return result;
}

QImage mapReadbackBufferRgba16(WGPUDevice device,
                               WGPUBuffer buffer,
                               const QSize &size,
                               const uint32_t paddedRowBytes,
                               QString *error)
{
    const uint64_t bufferSize = static_cast<uint64_t>(paddedRowBytes)
        * static_cast<uint32_t>(size.height());
    auto mapState = std::make_unique<MapState>();
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = onBufferMapped;
    callback.userdata1 = mapState.get();
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, bufferSize, callback);
#ifdef VFXPHOTOLAB_HAS_WGPU_NATIVE_POLL
    wgpuDevicePoll(device, true, nullptr);
#endif
    {
        std::unique_lock lock(mapState->mutex);
        if (!mapState->condition.wait_for(lock, kCallbackTimeout,
                                          [&] { return mapState->complete; })) {
            mapState->abandoned = true;
            mapState.release();
            if (error) *error = QStringLiteral(
                "Timed out waiting for GPU 16-bit display readback");
            return {};
        }
    }
    if (mapState->status != WGPUMapAsyncStatus_Success) {
        if (error) {
            *error = QStringLiteral("GPU 16-bit display readback failed: %1")
                         .arg(mapState->message.isEmpty()
                                  ? QStringLiteral("unknown map error")
                                  : mapState->message);
        }
        return {};
    }
    const void *mapped = wgpuBufferGetConstMappedRange(buffer, 0, bufferSize);
    if (!mapped) {
        if (error) *error = QStringLiteral(
            "GPU 16-bit display readback returned no mapped data");
        wgpuBufferUnmap(buffer);
        return {};
    }
    QImage result(size, QImage::Format_RGBA64);
    if (result.isNull()) {
        if (error) *error = QStringLiteral(
            "GPU 16-bit display image allocation failed");
        wgpuBufferUnmap(buffer);
        return {};
    }
    for (int y = 0; y < size.height(); ++y) {
        const auto *source = reinterpret_cast<const qfloat16 *>(
            static_cast<const char *>(mapped)
            + static_cast<size_t>(y) * paddedRowBytes);
        auto *destination = reinterpret_cast<QRgba64 *>(result.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int offset = x * 4;
            const auto component = [&](const int index) {
                return static_cast<quint16>(std::clamp(
                    std::lround(static_cast<float>(source[offset + index])
                                * 65535.0f),
                    0L, 65535L));
            };
            destination[x] = QRgba64::fromRgba64(
                component(0), component(1), component(2), component(3));
        }
    }
    wgpuBufferUnmap(buffer);
    return result;
}

WGPUShaderModule createShaderModule(WGPUDevice device,
                                    const char *label,
                                    const char *source,
                                    QString *error = nullptr)
{
    const QString reservedWord = firstReservedWgslWord(source);
    if (!reservedWord.isEmpty()) {
        const QString message = QStringLiteral(
            "WGSL shader '%1' contains reserved identifier '%2'")
                                    .arg(QString::fromUtf8(label ? label : "unnamed shader"), reservedWord);
        if (error) {
            *error = message;
        }
        qWarning().noquote() << "[GPU diagnostic]" << message;
        return nullptr;
    }

    const QString swizzle = firstUnsupportedWgslSwizzleAssignment(source);
    if (!swizzle.isEmpty()) {
        const QString message = QStringLiteral(
            "WGSL shader '%1' assigns to unsupported multi-component swizzle '%2'; "
            "construct and assign the complete vector instead")
                                    .arg(QString::fromUtf8(label ? label : "unnamed shader"), swizzle);
        if (error) {
            *error = message;
        }
        qWarning().noquote() << "[GPU diagnostic]" << message;
        return nullptr;
    }

    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code = stringView(source);
    WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    descriptor.label = stringView(label);
    descriptor.nextInChain = &wgsl.chain;
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}

uint32_t gpuBlendMode(const BlendMode mode)
{
    switch (mode) {
    case BlendMode::Copy: return 0;
    case BlendMode::Multiply: return 1;
    case BlendMode::Screen: return 2;
    case BlendMode::Overlay: return 3;
    case BlendMode::Darken: return 4;
    case BlendMode::Lighten: return 5;
    case BlendMode::ColourDodge: return 6;
    case BlendMode::ColourBurn: return 7;
    case BlendMode::Add: return 8;
    case BlendMode::Subtract: return 9;
    case BlendMode::Difference: return 10;
    case BlendMode::Exclusion: return 11;
    }
    return 0;
}

uint32_t gpuAdjustmentType(const AdjustmentType type)
{
    switch (type) {
    case AdjustmentType::Exposure: return 0;
    case AdjustmentType::Contrast: return 1;
    case AdjustmentType::Saturation: return 2;
    case AdjustmentType::Levels: return 3;
    case AdjustmentType::Curves: return 4;
    case AdjustmentType::HueSaturation: return 5;
    case AdjustmentType::Vibrance: return 6;
    case AdjustmentType::WhiteBalance: return 7;
    case AdjustmentType::ColourBalance: return 8;
    case AdjustmentType::ChannelMixer: return 9;
    case AdjustmentType::BlackAndWhite: return 10;
    case AdjustmentType::GradientMap: return 11;
    case AdjustmentType::Posterise: return 12;
    case AdjustmentType::Threshold: return 13;
    case AdjustmentType::Lut: return 14;
    case AdjustmentType::ShadowsHighlights: return 15;
    case AdjustmentType::GaussianBlur: return 16;
    case AdjustmentType::BoxBlur: return 17;
    case AdjustmentType::UnsharpMask: return 18;
    case AdjustmentType::HighPass: return 19;
    case AdjustmentType::Invert: return 20;
    case AdjustmentType::PhotoFilter: return 21;
    case AdjustmentType::SelectiveColour: return 22;
    case AdjustmentType::Vignette: return 23;
    case AdjustmentType::RgbSplit: return 24;
    case AdjustmentType::ChromaticAberrationCorrection: return 25;
    case AdjustmentType::SurfaceBlur: return 26;
    case AdjustmentType::MotionBlur: return 27;
    case AdjustmentType::RadialBlur: return 28;
    }
    return 0;
}

#endif

} // namespace

struct WebGpuContext::Impl {
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    struct ResidentTile {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        quint64 revision = 0;
        QSize size;
        qsizetype bytes = 0;
        quint64 lastUseSerial = 0;
    };

    struct DisplayLutResident {
        WGPUTexture forwardTexture = nullptr;
        WGPUTextureView forwardView = nullptr;
        WGPUTexture roundTripTexture = nullptr;
        WGPUTextureView roundTripView = nullptr;
        qsizetype bytes = 0;
        quint64 lastUseSerial = 0;
    };

    struct ManagedAdjustmentLutResident {
        WGPUTexture workingToDomainTexture = nullptr;
        WGPUTextureView workingToDomainView = nullptr;
        WGPUTexture domainToWorkingTexture = nullptr;
        WGPUTextureView domainToWorkingView = nullptr;
        qsizetype bytes = 0;
        quint64 lastUseSerial = 0;
    };

    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUShaderModule compositeShader = nullptr;
    WGPUComputePipeline compositePipeline = nullptr;
    WGPUShaderModule adjustmentShader = nullptr;
    WGPUComputePipeline adjustmentPipeline = nullptr;
    WGPUComputePipeline shadowsHorizontalPipeline = nullptr;
    WGPUComputePipeline shadowsApplyPipeline = nullptr;
    WGPUShaderModule passThroughShader = nullptr;
    WGPUComputePipeline passThroughPipeline = nullptr;
    WGPUShaderModule brushShader = nullptr;
    WGPUComputePipeline brushPipeline = nullptr;
    WGPUShaderModule fillShader = nullptr;
    WGPUComputePipeline fillPipeline = nullptr;
    WGPUShaderModule vectorFeatherShader = nullptr;
    WGPUComputePipeline vectorFeatherHorizontalPipeline = nullptr;
    WGPUComputePipeline vectorFeatherVerticalPipeline = nullptr;
    WGPUShaderModule gradientShader = nullptr;
    WGPUComputePipeline gradientPipeline = nullptr;
    WGPUShaderModule cloneShader = nullptr;
    WGPUComputePipeline clonePipeline = nullptr;
    WGPUShaderModule resampleShader = nullptr;
    WGPUComputePipeline resamplePipeline = nullptr;
    WGPUShaderModule transformPreviewShader = nullptr;
    WGPUComputePipeline transformPreviewPipeline = nullptr;
    WGPUShaderModule displayColourShader = nullptr;
    WGPUComputePipeline displayColourPipeline8 = nullptr;
    WGPUComputePipeline displayColourPipeline16 = nullptr;
    QHash<quint64, ResidentTile> residentTiles;
    QHash<QByteArray, DisplayLutResident> displayLuts;
    QHash<QByteArray, ManagedAdjustmentLutResident> managedAdjustmentLuts;
    qsizetype displayLutBytes = 0;
    qsizetype displayLutBudget = qsizetype(48) * 1024 * 1024;
    quint64 displayLutUseSerial = 0;
    qsizetype managedAdjustmentLutBytes = 0;
    qsizetype managedAdjustmentLutBudget = qsizetype(48) * 1024 * 1024;
    quint64 managedAdjustmentLutUseSerial = 0;
    qsizetype residentBytes = 0;
    qsizetype residentBudget = qsizetype(512) * 1024 * 1024;
    quint64 residentUseSerial = 0;

    void removeResidentTile(const quint64 key)
    {
        auto iterator = residentTiles.find(key);
        if (iterator == residentTiles.end()) return;
        ResidentTile tile = iterator.value();
        residentBytes -= tile.bytes;
        residentTiles.erase(iterator);
        if (tile.view) wgpuTextureViewRelease(tile.view);
        if (tile.texture) wgpuTextureRelease(tile.texture);
    }

    void clearResidentTiles()
    {
        const QList<quint64> keys = residentTiles.keys();
        for (const quint64 key : keys) removeResidentTile(key);
        residentBytes = 0;
    }

    void evictResidentTiles()
    {
        while (residentBytes > residentBudget && !residentTiles.isEmpty()) {
            auto victim = residentTiles.cbegin();
            for (auto iterator = residentTiles.cbegin(); iterator != residentTiles.cend(); ++iterator) {
                if (iterator.value().lastUseSerial < victim.value().lastUseSerial
                    || (iterator.value().lastUseSerial == victim.value().lastUseSerial
                        && iterator.key() < victim.key())) {
                    victim = iterator;
                }
            }
            removeResidentTile(victim.key());
        }
    }


    void removeDisplayLut(const QByteArray &key)
    {
        auto iterator = displayLuts.find(key);
        if (iterator == displayLuts.end()) return;
        DisplayLutResident lut = iterator.value();
        displayLutBytes -= lut.bytes;
        displayLuts.erase(iterator);
        if (lut.roundTripView) wgpuTextureViewRelease(lut.roundTripView);
        if (lut.roundTripTexture) wgpuTextureRelease(lut.roundTripTexture);
        if (lut.forwardView) wgpuTextureViewRelease(lut.forwardView);
        if (lut.forwardTexture) wgpuTextureRelease(lut.forwardTexture);
    }

    void clearDisplayLuts()
    {
        const QList<QByteArray> keys = displayLuts.keys();
        for (const QByteArray &key : keys) removeDisplayLut(key);
        displayLutBytes = 0;
    }

    void evictDisplayLuts()
    {
        while (displayLutBytes > displayLutBudget && displayLuts.size() > 1) {
            auto victim = displayLuts.cbegin();
            for (auto iterator = displayLuts.cbegin(); iterator != displayLuts.cend(); ++iterator) {
                if (iterator.value().lastUseSerial < victim.value().lastUseSerial) {
                    victim = iterator;
                }
            }
            removeDisplayLut(victim.key());
        }
    }

    void removeManagedAdjustmentLut(const QByteArray &key)
    {
        auto iterator = managedAdjustmentLuts.find(key);
        if (iterator == managedAdjustmentLuts.end()) return;
        ManagedAdjustmentLutResident lut = iterator.value();
        managedAdjustmentLutBytes -= lut.bytes;
        managedAdjustmentLuts.erase(iterator);
        if (lut.domainToWorkingView) wgpuTextureViewRelease(lut.domainToWorkingView);
        if (lut.domainToWorkingTexture) wgpuTextureRelease(lut.domainToWorkingTexture);
        if (lut.workingToDomainView) wgpuTextureViewRelease(lut.workingToDomainView);
        if (lut.workingToDomainTexture) wgpuTextureRelease(lut.workingToDomainTexture);
    }

    void clearManagedAdjustmentLuts()
    {
        const QList<QByteArray> keys = managedAdjustmentLuts.keys();
        for (const QByteArray &key : keys) removeManagedAdjustmentLut(key);
        managedAdjustmentLutBytes = 0;
    }

    void evictManagedAdjustmentLuts()
    {
        while (managedAdjustmentLutBytes > managedAdjustmentLutBudget
               && managedAdjustmentLuts.size() > 1) {
            auto victim = managedAdjustmentLuts.cbegin();
            for (auto iterator = managedAdjustmentLuts.cbegin();
                 iterator != managedAdjustmentLuts.cend(); ++iterator) {
                if (iterator.value().lastUseSerial < victim.value().lastUseSerial) {
                    victim = iterator;
                }
            }
            removeManagedAdjustmentLut(victim.key());
        }
    }
#endif
    QString status = QStringLiteral(
        "wgpu-native SDK was not linked; CPU renderer active. Run the bundled SDK fetch script and rebuild.");
    bool initialised = false;
    bool selfTestPassed = false;
    bool fillSelfTestPassed = false;
    bool gradientSelfTestPassed = false;
    bool vectorFeatherSelfTestPassed = false;
    bool displayTransformSelfTestPassed = false;
    bool managedAdjustmentTransformSelfTestPassed = false;
    quint32 approvedAdjustmentMask = 0;
    UncapturedErrorState uncapturedError;
    mutable std::mutex operationMutex;

    void releaseResources()
    {
#ifdef VFXPHOTOLAB_HAS_WEBGPU
#  ifdef VFXPHOTOLAB_HAS_WGPU_NATIVE_POLL
        // Finish all submitted work and deliver any pending callbacks before
        // releasing objects. In particular, do not leave spontaneous map
        // callbacks racing the C++/Qt static-destruction boundary.
        if (device) {
            wgpuDevicePoll(device, true, nullptr);
        }
#  endif
        clearResidentTiles();
        clearDisplayLuts();
        clearManagedAdjustmentLuts();
        if (displayColourPipeline16) wgpuComputePipelineRelease(displayColourPipeline16);
        displayColourPipeline16 = nullptr;
        if (displayColourPipeline8) wgpuComputePipelineRelease(displayColourPipeline8);
        displayColourPipeline8 = nullptr;
        if (displayColourShader) wgpuShaderModuleRelease(displayColourShader);
        displayColourShader = nullptr;
        if (transformPreviewPipeline) wgpuComputePipelineRelease(transformPreviewPipeline);
        transformPreviewPipeline = nullptr;
        if (transformPreviewShader) wgpuShaderModuleRelease(transformPreviewShader);
        transformPreviewShader = nullptr;
        if (resamplePipeline) wgpuComputePipelineRelease(resamplePipeline);
        resamplePipeline = nullptr;
        if (resampleShader) wgpuShaderModuleRelease(resampleShader);
        resampleShader = nullptr;
        if (clonePipeline) wgpuComputePipelineRelease(clonePipeline);
        clonePipeline = nullptr;
        if (cloneShader) wgpuShaderModuleRelease(cloneShader);
        cloneShader = nullptr;
        if (brushPipeline) wgpuComputePipelineRelease(brushPipeline);
        brushPipeline = nullptr;
        if (brushShader) wgpuShaderModuleRelease(brushShader);
        brushShader = nullptr;
        if (gradientPipeline) wgpuComputePipelineRelease(gradientPipeline);
        gradientPipeline = nullptr;
        if (gradientShader) wgpuShaderModuleRelease(gradientShader);
        gradientShader = nullptr;
        if (vectorFeatherVerticalPipeline) wgpuComputePipelineRelease(vectorFeatherVerticalPipeline);
        vectorFeatherVerticalPipeline = nullptr;
        if (vectorFeatherHorizontalPipeline) wgpuComputePipelineRelease(vectorFeatherHorizontalPipeline);
        vectorFeatherHorizontalPipeline = nullptr;
        if (vectorFeatherShader) wgpuShaderModuleRelease(vectorFeatherShader);
        vectorFeatherShader = nullptr;
        if (fillPipeline) wgpuComputePipelineRelease(fillPipeline);
        fillPipeline = nullptr;
        if (fillShader) wgpuShaderModuleRelease(fillShader);
        fillShader = nullptr;
        if (passThroughPipeline) wgpuComputePipelineRelease(passThroughPipeline);
        passThroughPipeline = nullptr;
        if (passThroughShader) wgpuShaderModuleRelease(passThroughShader);
        passThroughShader = nullptr;
        if (shadowsApplyPipeline) wgpuComputePipelineRelease(shadowsApplyPipeline);
        shadowsApplyPipeline = nullptr;
        if (shadowsHorizontalPipeline) wgpuComputePipelineRelease(shadowsHorizontalPipeline);
        shadowsHorizontalPipeline = nullptr;
        if (adjustmentPipeline) wgpuComputePipelineRelease(adjustmentPipeline);
        adjustmentPipeline = nullptr;
        if (adjustmentShader) wgpuShaderModuleRelease(adjustmentShader);
        adjustmentShader = nullptr;
        if (compositePipeline) wgpuComputePipelineRelease(compositePipeline);
        compositePipeline = nullptr;
        if (compositeShader) wgpuShaderModuleRelease(compositeShader);
        compositeShader = nullptr;
        if (queue) wgpuQueueRelease(queue);
        queue = nullptr;
        if (device) wgpuDeviceRelease(device);
        device = nullptr;
        if (adapter) wgpuAdapterRelease(adapter);
        adapter = nullptr;
        if (instance) wgpuInstanceRelease(instance);
        instance = nullptr;
#endif
        initialised = false;
        selfTestPassed = false;
        fillSelfTestPassed = false;
        gradientSelfTestPassed = false;
        vectorFeatherSelfTestPassed = false;
        displayTransformSelfTestPassed = false;
        managedAdjustmentTransformSelfTestPassed = false;
        approvedAdjustmentMask = 0;
    }

    ~Impl()
    {
        releaseResources();
    }
};

WebGpuContext::WebGpuContext()
    : m_impl(std::make_unique<Impl>())
{
}

WebGpuContext::~WebGpuContext() = default;

void WebGpuContext::shutdown()
{
    std::lock_guard operationLock(m_impl->operationMutex);
    m_impl->releaseResources();
}

bool WebGpuContext::compiledIn() const
{
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    return true;
#else
    return false;
#endif
}

bool WebGpuContext::initialise()
{
    std::lock_guard operationLock(m_impl->operationMutex);
    if (m_impl->initialised) {
        return true;
    }

#ifdef VFXPHOTOLAB_HAS_WEBGPU
    qInfo().noquote() << "[GPU diagnostic] Creating wgpu-native instance.";
    WGPUInstanceDescriptor instanceDescriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
    m_impl->instance = wgpuCreateInstance(&instanceDescriptor);
    if (!m_impl->instance) {
        m_impl->status = QStringLiteral("wgpu-native was linked, but the WebGPU instance could not be created; CPU renderer active");
        return false;
    }

    qInfo().noquote() << "[GPU diagnostic] Requesting high-performance adapter.";
    auto adapterState = std::make_unique<RequestState<WGPUAdapter, WGPURequestAdapterStatus>>();
    WGPURequestAdapterOptions adapterOptions = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo adapterCallback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapterCallback.mode = WGPUCallbackMode_AllowSpontaneous;
    adapterCallback.callback = onAdapterRequested;
    adapterCallback.userdata1 = adapterState.get();
    wgpuInstanceRequestAdapter(m_impl->instance, &adapterOptions, adapterCallback);

    {
        std::unique_lock lock(adapterState->mutex);
        if (!adapterState->condition.wait_for(lock, kCallbackTimeout, [&] { return adapterState->complete; })) {
            adapterState->abandoned = true;
            adapterState.release(); // the eventual callback owns and deletes the state
            m_impl->status = QStringLiteral("Timed out while requesting a WebGPU adapter; CPU renderer active");
            return false;
        }
    }
    if (adapterState->status != WGPURequestAdapterStatus_Success || !adapterState->handle) {
        m_impl->status = QStringLiteral("No usable WebGPU adapter: %1; CPU renderer active")
                             .arg(adapterState->message.isEmpty() ? QStringLiteral("unknown adapter error")
                                                                 : adapterState->message);
        return false;
    }
    m_impl->adapter = adapterState->handle;
    qInfo().noquote() << "[GPU diagnostic] Adapter ready; requesting device.";

    auto deviceState = std::make_unique<RequestState<WGPUDevice, WGPURequestDeviceStatus>>();
    WGPUDeviceDescriptor deviceDescriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    deviceDescriptor.label = stringView("VFX Photo Lab WebGPU device");
    deviceDescriptor.uncapturedErrorCallbackInfo.callback = onUncapturedError;
    deviceDescriptor.uncapturedErrorCallbackInfo.userdata1 = &m_impl->uncapturedError;
    WGPURequestDeviceCallbackInfo deviceCallback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    deviceCallback.mode = WGPUCallbackMode_AllowSpontaneous;
    deviceCallback.callback = onDeviceRequested;
    deviceCallback.userdata1 = deviceState.get();
    wgpuAdapterRequestDevice(m_impl->adapter, &deviceDescriptor, deviceCallback);

    {
        std::unique_lock lock(deviceState->mutex);
        if (!deviceState->condition.wait_for(lock, kCallbackTimeout, [&] { return deviceState->complete; })) {
            deviceState->abandoned = true;
            deviceState.release(); // the eventual callback owns and deletes the state
            m_impl->status = QStringLiteral("Timed out while requesting a WebGPU device; CPU renderer active");
            return false;
        }
    }
    if (deviceState->status != WGPURequestDeviceStatus_Success || !deviceState->handle) {
        m_impl->status = QStringLiteral("WebGPU device creation failed: %1; CPU renderer active")
                             .arg(deviceState->message.isEmpty() ? QStringLiteral("unknown device error")
                                                                : deviceState->message);
        return false;
    }
    m_impl->device = deviceState->handle;
    qInfo().noquote() << "[GPU diagnostic] Device ready; acquiring default queue.";
    m_impl->queue = wgpuDeviceGetQueue(m_impl->device);
    if (!m_impl->queue) {
        m_impl->status = QStringLiteral("WebGPU device was created without a usable queue; CPU renderer active");
        return false;
    }

    m_impl->initialised = true;
    qInfo().noquote() << "[GPU diagnostic] Adapter, device and queue are ready.";
    m_impl->status = QStringLiteral("Native WebGPU adapter, device and queue ready; validating the isolated GPU tile path");
    return true;
#else
    return false;
#endif
}

bool WebGpuContext::isInitialised() const
{
    return m_impl->initialised;
}

bool WebGpuContext::deviceReady() const
{
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    return m_impl->initialised && m_impl->device && m_impl->queue;
#else
    return false;
#endif
}

QImage WebGpuContext::roundTripTile(const QImage &source, QString *error)
{
    std::lock_guard operationLock(m_impl->operationMutex);
    if (error) {
        error->clear();
    }
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    if (!deviceReady()) {
        if (error) {
            *error = QStringLiteral("WebGPU device is not ready");
        }
        return {};
    }
    if (source.isNull() || source.width() <= 0 || source.height() <= 0) {
        if (error) {
            *error = QStringLiteral("Tile is empty");
        }
        return {};
    }

    qInfo().noquote() << "[GPU diagnostic] Allocating and dispatching diagnostic tile.";
    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    const uint32_t width = static_cast<uint32_t>(rgba.width());
    const uint32_t height = static_cast<uint32_t>(rgba.height());
    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const uint64_t bufferSize = static_cast<uint64_t>(paddedRowBytes) * height;

    QByteArray upload(static_cast<qsizetype>(bufferSize), '\0');
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(upload.data() + static_cast<qsizetype>(y * paddedRowBytes),
                    rgba.constScanLine(static_cast<int>(y)),
                    static_cast<size_t>(width) * 4);
    }

    WGPUTexture sourceTexture = nullptr;
    WGPUTexture destinationTexture = nullptr;
    WGPUTextureView sourceView = nullptr;
    WGPUTextureView destinationView = nullptr;
    WGPUShaderModule shader = nullptr;
    WGPUComputePipeline pipeline = nullptr;
    WGPUBindGroupLayout bindGroupLayout = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;

    auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (bindGroupLayout) wgpuBindGroupLayoutRelease(bindGroupLayout);
        if (pipeline) wgpuComputePipelineRelease(pipeline);
        if (shader) wgpuShaderModuleRelease(shader);
        if (destinationView) wgpuTextureViewRelease(destinationView);
        if (sourceView) wgpuTextureViewRelease(sourceView);
        if (destinationTexture) wgpuTextureRelease(destinationTexture);
        if (sourceTexture) wgpuTextureRelease(sourceTexture);
    };

    WGPUTextureDescriptor sourceDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    sourceDescriptor.label = stringView("PhotoLab parity source tile");
    sourceDescriptor.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
    sourceDescriptor.dimension = WGPUTextureDimension_2D;
    sourceDescriptor.size = {width, height, 1};
    sourceDescriptor.format = WGPUTextureFormat_RGBA8Unorm;
    sourceDescriptor.mipLevelCount = 1;
    sourceDescriptor.sampleCount = 1;
    sourceTexture = wgpuDeviceCreateTexture(m_impl->device, &sourceDescriptor);

    WGPUTextureDescriptor destinationDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    destinationDescriptor.label = stringView("PhotoLab parity destination tile");
    destinationDescriptor.usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc;
    destinationDescriptor.dimension = WGPUTextureDimension_2D;
    destinationDescriptor.size = {width, height, 1};
    destinationDescriptor.format = WGPUTextureFormat_RGBA8Unorm;
    destinationDescriptor.mipLevelCount = 1;
    destinationDescriptor.sampleCount = 1;
    destinationTexture = wgpuDeviceCreateTexture(m_impl->device, &destinationDescriptor);
    if (!sourceTexture || !destinationTexture) {
        if (error) *error = QStringLiteral("GPU texture allocation failed");
        cleanup();
        return {};
    }

    sourceView = wgpuTextureCreateView(sourceTexture, nullptr);
    destinationView = wgpuTextureCreateView(destinationTexture, nullptr);
    if (!sourceView || !destinationView) {
        if (error) *error = QStringLiteral("GPU texture view creation failed");
        cleanup();
        return {};
    }

    WGPUTexelCopyTextureInfo uploadDestination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    uploadDestination.texture = sourceTexture;
    WGPUTexelCopyBufferLayout uploadLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    uploadLayout.bytesPerRow = paddedRowBytes;
    uploadLayout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuQueueWriteTexture(m_impl->queue,
                          &uploadDestination,
                          upload.constData(),
                          static_cast<size_t>(upload.size()),
                          &uploadLayout,
                          &extent);

    static constexpr char shaderSource[] = R"WGSL(
@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var destination_tile: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(source_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }
    let pixel = textureLoad(source_tile, vec2<i32>(gid.xy), 0);
    textureStore(destination_tile, vec2<i32>(gid.xy), pixel);
}
)WGSL";

    shader = createShaderModule(m_impl->device,
                                "PhotoLab identity tile shader",
                                shaderSource,
                                error);
    if (!shader) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("WGSL shader creation failed");
        }
        cleanup();
        return {};
    }

    WGPUComputePipelineDescriptor pipelineDescriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    pipelineDescriptor.label = stringView("PhotoLab identity tile pipeline");
    pipelineDescriptor.compute.module = shader;
    pipelineDescriptor.compute.entryPoint = stringView("main");
    pipeline = wgpuDeviceCreateComputePipeline(m_impl->device, &pipelineDescriptor);
    if (!pipeline) {
        if (error) *error = QStringLiteral("GPU compute pipeline creation failed");
        cleanup();
        return {};
    }

    bindGroupLayout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    if (!bindGroupLayout) {
        if (error) *error = QStringLiteral("GPU bind group layout creation failed");
        cleanup();
        return {};
    }
    WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = sourceView;
    entries[1].binding = 1;
    entries[1].textureView = destinationView;
    WGPUBindGroupDescriptor bindGroupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindGroupDescriptor.label = stringView("PhotoLab identity tile bind group");
    bindGroupDescriptor.layout = bindGroupLayout;
    bindGroupDescriptor.entryCount = 2;
    bindGroupDescriptor.entries = entries;
    bindGroup = wgpuDeviceCreateBindGroup(m_impl->device, &bindGroupDescriptor);
    if (!bindGroup) {
        if (error) *error = QStringLiteral("GPU bind group creation failed");
        cleanup();
        return {};
    }

    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab tile readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = bufferSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!readbackBuffer) {
        if (error) *error = QStringLiteral("GPU readback buffer allocation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("PhotoLab tile command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    if (!encoder) {
        if (error) *error = QStringLiteral("GPU command encoder creation failed");
        cleanup();
        return {};
    }
    WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab tile compute pass");
    pass = wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor);
    if (!pass) {
        if (error) *error = QStringLiteral("GPU compute pass creation failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = destinationTexture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource, &copyDestination, &extent);

    WGPUCommandBufferDescriptor commandDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab tile commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral("GPU command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);

    auto mapState = std::make_unique<MapState>();
    WGPUBufferMapCallbackInfo mapCallback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mapCallback.mode = WGPUCallbackMode_AllowSpontaneous;
    mapCallback.callback = onBufferMapped;
    mapCallback.userdata1 = mapState.get();
    qInfo().noquote() << "[GPU diagnostic] Waiting for diagnostic tile readback.";
    wgpuBufferMapAsync(readbackBuffer, WGPUMapMode_Read, 0, bufferSize, mapCallback);
#ifdef VFXPHOTOLAB_HAS_WGPU_NATIVE_POLL
    // wgpu-native's own compute example explicitly polls the device after an
    // asynchronous map request. Without this, some Linux/Vulkan drivers never
    // deliver the map callback even though the submitted work has completed.
    wgpuDevicePoll(m_impl->device, true, nullptr);
#endif
    {
        std::unique_lock lock(mapState->mutex);
        if (!mapState->condition.wait_for(lock, kCallbackTimeout, [&] { return mapState->complete; })) {
            mapState->abandoned = true;
            mapState.release(); // the eventual callback owns and deletes the state
            if (error) *error = QStringLiteral("Timed out waiting for GPU tile readback");
            cleanup();
            return {};
        }
    }
    if (mapState->status != WGPUMapAsyncStatus_Success) {
        if (error) {
            *error = QStringLiteral("GPU tile readback failed: %1")
                         .arg(mapState->message.isEmpty() ? QStringLiteral("unknown map error")
                                                        : mapState->message);
        }
        cleanup();
        return {};
    }

    const void *mapped = wgpuBufferGetConstMappedRange(readbackBuffer, 0, bufferSize);
    if (!mapped) {
        if (error) *error = QStringLiteral("GPU readback returned no mapped data");
        wgpuBufferUnmap(readbackBuffer);
        cleanup();
        return {};
    }

    QImage result(rgba.size(), QImage::Format_RGBA8888);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(result.scanLine(static_cast<int>(y)),
                    static_cast<const char *>(mapped) + static_cast<size_t>(y) * paddedRowBytes,
                    static_cast<size_t>(width) * 4);
    }
    wgpuBufferUnmap(readbackBuffer);
    cleanup();
    qInfo().noquote() << "[GPU diagnostic] Diagnostic tile readback completed.";
    return result;
#else
    Q_UNUSED(source)
    if (error) {
        *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    }
    return {};
#endif
}

QImage WebGpuContext::transformPreviewComposite(
    const QImage &background,
    const QImage &foreground,
    const QTransform &previewTransform,
    QString *error)
{
    if (error) {
        error->clear();
    }
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (background.isNull() || foreground.isNull()
        || background.size() != foreground.size()
        || background.depth() > 32 || foreground.depth() > 32) {
        if (error) {
            *error = QStringLiteral(
                "GPU transform preview requires matching 8-bit surfaces");
        }
        return {};
    }
    bool invertible = false;
    const QTransform inverse = previewTransform.inverted(&invertible);
    if (!invertible) {
        if (error) *error = QStringLiteral("Transform preview matrix is singular");
        return {};
    }

    const QImage backgroundRgba = background.convertToFormat(
        QImage::Format_RGBA8888_Premultiplied);
    const QImage foregroundRgba = foreground.convertToFormat(
        QImage::Format_RGBA8888_Premultiplied);
    if (backgroundRgba.isNull() || foregroundRgba.isNull()) {
        if (error) *error = QStringLiteral("Transform preview conversion failed");
        return {};
    }

    static constexpr char transformShaderSource[] = R"WGSL(
struct TransformParams {
    inverse_x: vec4<f32>,
    inverse_y: vec4<f32>,
    inverse_w: vec4<f32>,
    output_size: vec2<u32>,
    foreground_size: vec2<u32>,
};

@group(0) @binding(0) var background_texture: texture_2d<f32>;
@group(0) @binding(1) var foreground_texture: texture_2d<f32>;
@group(0) @binding(2) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: TransformParams;

fn map_to_source(point: vec2<f32>) -> vec2<f32> {
    let homogeneous = vec3<f32>(point, 1.0);
    let denominator = dot(params.inverse_w.xyz, homogeneous);
    if (abs(denominator) < 1.0e-7) {
        return vec2<f32>(-1.0e20);
    }
    return vec2<f32>(dot(params.inverse_x.xyz, homogeneous),
                     dot(params.inverse_y.xyz, homogeneous)) / denominator;
}

fn load_foreground(position: vec2<i32>) -> vec4<f32> {
    let dimensions = vec2<i32>(params.foreground_size);
    if (any(position < vec2<i32>(0)) || any(position >= dimensions)) {
        return vec4<f32>(0.0);
    }
    return textureLoad(foreground_texture, position, 0);
}

fn bilinear_foreground(position: vec2<f32>) -> vec4<f32> {
    let base = vec2<i32>(floor(position));
    let fraction = fract(position);
    let top = mix(load_foreground(base),
                  load_foreground(base + vec2<i32>(1, 0)), fraction.x);
    let bottom = mix(load_foreground(base + vec2<i32>(0, 1)),
                     load_foreground(base + vec2<i32>(1, 1)), fraction.x);
    return mix(top, bottom, fraction.y);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.output_size.x || gid.y >= params.output_size.y) {
        return;
    }
    let output_position = vec2<i32>(gid.xy);
    let background = textureLoad(background_texture, output_position, 0);
    let source_position = map_to_source(vec2<f32>(gid.xy) + vec2<f32>(0.5))
        - vec2<f32>(0.5);
    let foreground = bilinear_foreground(source_position);
    textureStore(output_texture, output_position,
                 foreground + background * (1.0 - foreground.a));
}
)WGSL";

    if (!m_impl->transformPreviewPipeline) {
        if (m_impl->transformPreviewShader) {
            wgpuShaderModuleRelease(m_impl->transformPreviewShader);
            m_impl->transformPreviewShader = nullptr;
        }
        m_impl->transformPreviewShader = createShaderModule(
            m_impl->device,
            "PhotoLab projective transform preview shader",
            transformShaderSource);
        if (m_impl->transformPreviewShader) {
            WGPUComputePipelineDescriptor descriptor =
                WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView(
                "PhotoLab projective transform preview pipeline");
            descriptor.compute.module = m_impl->transformPreviewShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->transformPreviewPipeline = wgpuDeviceCreateComputePipeline(
                m_impl->device, &descriptor);
        }
        if (!m_impl->transformPreviewPipeline) {
            if (error) *error = QStringLiteral(
                "GPU transform preview pipeline creation failed");
            return {};
        }
    }

    struct alignas(16) TransformParams {
        float inverseX[4];
        float inverseY[4];
        float inverseW[4];
        uint32_t outputSize[2];
        uint32_t foregroundSize[2];
    };
    TransformParams params {};
    params.inverseX[0] = static_cast<float>(inverse.m11());
    params.inverseX[1] = static_cast<float>(inverse.m21());
    params.inverseX[2] = static_cast<float>(inverse.m31());
    params.inverseY[0] = static_cast<float>(inverse.m12());
    params.inverseY[1] = static_cast<float>(inverse.m22());
    params.inverseY[2] = static_cast<float>(inverse.m32());
    params.inverseW[0] = static_cast<float>(inverse.m13());
    params.inverseW[1] = static_cast<float>(inverse.m23());
    params.inverseW[2] = static_cast<float>(inverse.m33());
    params.outputSize[0] = static_cast<uint32_t>(backgroundRgba.width());
    params.outputSize[1] = static_cast<uint32_t>(backgroundRgba.height());
    params.foregroundSize[0] = static_cast<uint32_t>(foregroundRgba.width());
    params.foregroundSize[1] = static_cast<uint32_t>(foregroundRgba.height());

    WGPUTexture backgroundTexture = nullptr;
    WGPUTexture foregroundTexture = nullptr;
    WGPUTexture outputTexture = nullptr;
    WGPUTextureView backgroundView = nullptr;
    WGPUTextureView foregroundView = nullptr;
    WGPUTextureView outputView = nullptr;
    WGPUBuffer paramsBuffer = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;

    const auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
        if (outputView) wgpuTextureViewRelease(outputView);
        if (foregroundView) wgpuTextureViewRelease(foregroundView);
        if (backgroundView) wgpuTextureViewRelease(backgroundView);
        if (outputTexture) wgpuTextureRelease(outputTexture);
        if (foregroundTexture) wgpuTextureRelease(foregroundTexture);
        if (backgroundTexture) wgpuTextureRelease(backgroundTexture);
    };

    backgroundTexture = uploadTexture(m_impl->device, m_impl->queue,
                                      backgroundRgba,
                                      "PhotoLab transform preview background",
                                      false, error);
    foregroundTexture = uploadTexture(m_impl->device, m_impl->queue,
                                      foregroundRgba,
                                      "PhotoLab transform preview foreground",
                                      false, error);
    outputTexture = createWorkingTexture(m_impl->device,
                                         backgroundRgba.size(),
                                         "PhotoLab transform preview output",
                                         error);
    backgroundView = backgroundTexture
        ? wgpuTextureCreateView(backgroundTexture, nullptr) : nullptr;
    foregroundView = foregroundTexture
        ? wgpuTextureCreateView(foregroundTexture, nullptr) : nullptr;
    outputView = outputTexture
        ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
    if (!backgroundTexture || !foregroundTexture || !outputTexture
        || !backgroundView || !foregroundView || !outputView) {
        cleanup();
        return {};
    }

    WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramsDescriptor.label = stringView("PhotoLab transform preview parameters");
    paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramsDescriptor.size = sizeof(params);
    paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
    const uint32_t paddedRowBytes = alignedBytesPerRow(
        static_cast<uint32_t>(backgroundRgba.width()));
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes)
        * static_cast<uint32_t>(backgroundRgba.height());
    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab transform preview readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = readbackSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!paramsBuffer || !readbackBuffer) {
        if (error) *error = QStringLiteral(
            "GPU transform preview buffer allocation failed");
        cleanup();
        return {};
    }
    wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

    layout = wgpuComputePipelineGetBindGroupLayout(
        m_impl->transformPreviewPipeline, 0);
    WGPUBindGroupEntry entries[4] = {
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = backgroundView;
    entries[1].binding = 1; entries[1].textureView = foregroundView;
    entries[2].binding = 2; entries[2].textureView = outputView;
    entries[3].binding = 3; entries[3].buffer = paramsBuffer;
    entries[3].size = sizeof(params);
    WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindDescriptor.label = stringView("PhotoLab transform preview bind group");
    bindDescriptor.layout = layout;
    bindDescriptor.entryCount = 4;
    bindDescriptor.entries = entries;
    bindGroup = layout
        ? wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor) : nullptr;
    if (!layout || !bindGroup) {
        if (error) *error = QStringLiteral(
            "GPU transform preview bind group creation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView(
        "PhotoLab transform preview command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    WGPUComputePassDescriptor passDescriptor =
        WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab transform preview pass");
    pass = encoder
        ? wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor) : nullptr;
    if (!encoder || !pass) {
        if (error) *error = QStringLiteral(
            "GPU transform preview command encoding failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, m_impl->transformPreviewPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass,
        (params.outputSize[0] + 7) / 8,
        (params.outputSize[1] + 7) / 8,
        1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = outputTexture;
    WGPUTexelCopyBufferInfo copyDestination =
        WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = params.outputSize[1];
    const WGPUExtent3D extent {
        params.outputSize[0], params.outputSize[1], 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource,
                                          &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor =
        WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab transform preview commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral(
            "GPU transform preview command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage raw = mapReadbackBuffer(m_impl->device,
                                   readbackBuffer,
                                   backgroundRgba.size(),
                                   paddedRowBytes,
                                   error);
    cleanup();
    if (raw.isNull()) {
        return {};
    }
    QImage result(raw.size(), QImage::Format_RGBA8888_Premultiplied);
    if (result.isNull()) {
        if (error) *error = QStringLiteral(
            "Transform preview image allocation failed");
        return {};
    }
    for (int y = 0; y < result.height(); ++y) {
        std::memcpy(result.scanLine(y), raw.constScanLine(y),
                    static_cast<size_t>(result.width()) * 4);
    }
    result.setColorSpace(background.colorSpace());
    return result;
#else
    Q_UNUSED(background)
    Q_UNUSED(foreground)
    Q_UNUSED(previewTransform)
    if (error) {
        *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    }
    return {};
#endif
}

QImage WebGpuContext::applyDisplayColourTransform(
    const QImage &source,
    const DisplayGpuLutData &lut,
    const std::atomic_bool *cancelRequested,
    QString *error)
{
    if (error) error->clear();
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    const auto isCancelled = [cancelRequested] {
        return cancelRequested
            && cancelRequested->load(std::memory_order_acquire);
    };
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (source.isNull() || !lut.isValid() || lut.fingerprint.isEmpty()) {
        if (error) *error = QStringLiteral(
            "The GPU display-transform source or lattice is invalid");
        return {};
    }
    if (isCancelled()) {
        if (error) *error = QStringLiteral("Display transform cancelled");
        return {};
    }

    if (!m_impl->displayColourShader) {
        m_impl->displayColourShader = createShaderModule(
            m_impl->device,
            "PhotoLab display colour transform shader",
            gpu_shader::DisplayColourTransform,
            error);
    }
    const auto createPipeline = [&](WGPUComputePipeline *pipeline,
                                    const char *label,
                                    const char *entryPoint) {
        if (*pipeline) return true;
        if (!m_impl->displayColourShader) return false;
        WGPUComputePipelineDescriptor descriptor =
            WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        descriptor.label = stringView(label);
        descriptor.compute.module = m_impl->displayColourShader;
        descriptor.compute.entryPoint = stringView(entryPoint);
        *pipeline = wgpuDeviceCreateComputePipeline(m_impl->device, &descriptor);
        return *pipeline != nullptr;
    };
    const bool sixteenBit = source.depth() > 32;
    WGPUComputePipeline pipeline = nullptr;
    if (sixteenBit) {
        if (!createPipeline(&m_impl->displayColourPipeline16,
                            "PhotoLab RGBA16 display colour pipeline",
                            "apply_rgba16")) {
            if (error && error->isEmpty()) *error = QStringLiteral(
                "GPU RGBA16 display colour pipeline creation failed");
            return {};
        }
        pipeline = m_impl->displayColourPipeline16;
    } else {
        if (!createPipeline(&m_impl->displayColourPipeline8,
                            "PhotoLab RGBA8 display colour pipeline",
                            "apply_rgba8")) {
            if (error && error->isEmpty()) *error = QStringLiteral(
                "GPU RGBA8 display colour pipeline creation failed");
            return {};
        }
        pipeline = m_impl->displayColourPipeline8;
    }

    auto cacheIterator = m_impl->displayLuts.find(lut.fingerprint);
    if (cacheIterator == m_impl->displayLuts.end()) {
        LutGpuTextureData forward;
        forward.size = QSize(lut.edgeSize * lut.edgeSize, lut.edgeSize);
        forward.rgba16f = lut.forwardRgba16f;
        LutGpuTextureData roundTrip;
        if (lut.gamutWarning) {
            roundTrip.size = forward.size;
            roundTrip.rgba16f = lut.gamutRoundTripRgba16f;
        }

        WebGpuContext::Impl::DisplayLutResident resident;
        resident.forwardTexture = uploadLutTexture(
            m_impl->device, m_impl->queue, forward,
            "PhotoLab display transform forward LUT", error);
        resident.roundTripTexture = resident.forwardTexture && lut.gamutWarning
            ? uploadLutTexture(m_impl->device, m_impl->queue, roundTrip,
                               "PhotoLab display transform round-trip LUT", error)
            : nullptr;
        resident.forwardView = resident.forwardTexture
            ? wgpuTextureCreateView(resident.forwardTexture, nullptr) : nullptr;
        WGPUTexture roundTripViewTexture = lut.gamutWarning
            ? resident.roundTripTexture : resident.forwardTexture;
        resident.roundTripView = roundTripViewTexture
            ? wgpuTextureCreateView(roundTripViewTexture, nullptr) : nullptr;
        if (!resident.forwardTexture || !resident.forwardView
            || !resident.roundTripView
            || (lut.gamutWarning && !resident.roundTripTexture)) {
            if (resident.roundTripView) wgpuTextureViewRelease(resident.roundTripView);
            if (resident.forwardView) wgpuTextureViewRelease(resident.forwardView);
            if (resident.roundTripTexture) wgpuTextureRelease(resident.roundTripTexture);
            if (resident.forwardTexture) wgpuTextureRelease(resident.forwardTexture);
            if (error && error->isEmpty()) *error = QStringLiteral(
                "GPU display-transform LUT upload failed");
            return {};
        }
        resident.bytes = (lut.forwardRgba16f.size()
                          + (lut.gamutWarning
                                 ? lut.gamutRoundTripRgba16f.size() : 0))
            * qsizetype(sizeof(qfloat16));
        resident.lastUseSerial = ++m_impl->displayLutUseSerial;
        m_impl->displayLutBytes += resident.bytes;
        m_impl->displayLuts.insert(lut.fingerprint, resident);
        m_impl->evictDisplayLuts();
        cacheIterator = m_impl->displayLuts.find(lut.fingerprint);
        if (cacheIterator == m_impl->displayLuts.end()) {
            if (error) *error = QStringLiteral(
                "GPU display-transform LUT cache insertion failed");
            return {};
        }
    }
    cacheIterator->lastUseSerial = ++m_impl->displayLutUseSerial;

    const QImage::Format originalFormat = source.format();
    const QSize size = source.size();
    const uint32_t width = static_cast<uint32_t>(size.width());
    const uint32_t height = static_cast<uint32_t>(size.height());
    const uint32_t bytesPerPixel = sixteenBit ? 8u : 4u;
    const uint32_t paddedRowBytes = alignedBytesPerRow(width, bytesPerPixel);
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes) * height;

    WGPUTexture sourceTexture = nullptr;
    WGPUTexture outputTexture = nullptr;
    WGPUTextureView sourceView = nullptr;
    WGPUTextureView outputView = nullptr;
    WGPUBuffer paramsBuffer = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;

    const auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
        if (outputView) wgpuTextureViewRelease(outputView);
        if (sourceView) wgpuTextureViewRelease(sourceView);
        if (outputTexture) wgpuTextureRelease(outputTexture);
        if (sourceTexture) wgpuTextureRelease(sourceTexture);
    };

    const WGPUTextureFormat textureFormat = sixteenBit
        ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
    WGPUTextureDescriptor sourceDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    sourceDescriptor.label = stringView("PhotoLab display transform source");
    sourceDescriptor.usage = WGPUTextureUsage_CopyDst
        | WGPUTextureUsage_TextureBinding;
    sourceDescriptor.dimension = WGPUTextureDimension_2D;
    sourceDescriptor.size = {width, height, 1};
    sourceDescriptor.format = textureFormat;
    sourceDescriptor.mipLevelCount = 1;
    sourceDescriptor.sampleCount = 1;
    sourceTexture = wgpuDeviceCreateTexture(m_impl->device, &sourceDescriptor);

    WGPUTextureDescriptor outputDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    outputDescriptor.label = stringView("PhotoLab display transform output");
    outputDescriptor.usage = WGPUTextureUsage_CopySrc
        | WGPUTextureUsage_StorageBinding;
    outputDescriptor.dimension = WGPUTextureDimension_2D;
    outputDescriptor.size = {width, height, 1};
    outputDescriptor.format = textureFormat;
    outputDescriptor.mipLevelCount = 1;
    outputDescriptor.sampleCount = 1;
    outputTexture = wgpuDeviceCreateTexture(m_impl->device, &outputDescriptor);
    sourceView = sourceTexture
        ? wgpuTextureCreateView(sourceTexture, nullptr) : nullptr;
    outputView = outputTexture
        ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
    if (!sourceTexture || !outputTexture || !sourceView || !outputView) {
        if (error) *error = QStringLiteral(
            "GPU display-transform texture allocation failed");
        cleanup();
        return {};
    }

    QByteArray upload(static_cast<qsizetype>(readbackSize), '\0');
    QImage prepared = source.convertToFormat(
        sixteenBit ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (prepared.isNull()) {
        if (error) *error = QStringLiteral(
            "The display-transform image could not be prepared for GPU upload");
        cleanup();
        return {};
    }
    if (sixteenBit) {
        for (uint32_t y = 0; y < height; ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(
                prepared.constScanLine(static_cast<int>(y)));
            auto *target = reinterpret_cast<qfloat16 *>(
                upload.data() + static_cast<qsizetype>(y) * paddedRowBytes);
            for (uint32_t x = 0; x < width; ++x) {
                const int offset = static_cast<int>(x) * 4;
                target[offset] = qfloat16(static_cast<float>(row[x].red() / 65535.0));
                target[offset + 1] = qfloat16(static_cast<float>(row[x].green() / 65535.0));
                target[offset + 2] = qfloat16(static_cast<float>(row[x].blue() / 65535.0));
                target[offset + 3] = qfloat16(static_cast<float>(row[x].alpha() / 65535.0));
            }
        }
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(upload.data() + static_cast<qsizetype>(y) * paddedRowBytes,
                        prepared.constScanLine(static_cast<int>(y)),
                        static_cast<size_t>(width) * 4);
        }
    }
    WGPUTexelCopyTextureInfo uploadDestination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    uploadDestination.texture = sourceTexture;
    WGPUTexelCopyBufferLayout uploadLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    uploadLayout.bytesPerRow = paddedRowBytes;
    uploadLayout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuQueueWriteTexture(m_impl->queue, &uploadDestination,
                          upload.constData(), static_cast<size_t>(upload.size()),
                          &uploadLayout, &extent);

    struct alignas(16) DisplayParams {
        uint32_t edgeSize;
        uint32_t gamutWarning;
        float gamutThreshold;
        float padding;
    };
    static_assert(sizeof(DisplayParams) == 16);
    DisplayParams params {
        static_cast<uint32_t>(lut.edgeSize),
        lut.gamutWarning ? 1u : 0u,
        lut.gamutWarningThreshold,
        0.0f
    };
    WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramsDescriptor.label = stringView("PhotoLab display transform parameters");
    paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramsDescriptor.size = sizeof(params);
    paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab display transform readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = readbackSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!paramsBuffer || !readbackBuffer) {
        if (error) *error = QStringLiteral(
            "GPU display-transform buffer allocation failed");
        cleanup();
        return {};
    }
    wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

    layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    WGPUBindGroupEntry entries[5] = {
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].textureView = sourceView;
    entries[1].binding = 1;
    entries[1].textureView = cacheIterator->forwardView;
    entries[2].binding = 2;
    entries[2].textureView = cacheIterator->roundTripView;
    entries[3].binding = sixteenBit ? 4 : 3;
    entries[3].textureView = outputView;
    entries[4].binding = 5;
    entries[4].buffer = paramsBuffer;
    entries[4].size = sizeof(params);
    WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindDescriptor.label = stringView("PhotoLab display transform bind group");
    bindDescriptor.layout = layout;
    bindDescriptor.entryCount = 5;
    bindDescriptor.entries = entries;
    bindGroup = layout
        ? wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor) : nullptr;
    if (!layout || !bindGroup) {
        if (error) *error = QStringLiteral(
            "GPU display-transform bind group creation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView(
        "PhotoLab display transform command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab display transform pass");
    pass = encoder
        ? wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor) : nullptr;
    if (!encoder || !pass) {
        if (error) *error = QStringLiteral(
            "GPU display-transform command encoding failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = outputTexture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource,
                                          &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor =
        WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab display transform commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral(
            "GPU display-transform command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage result = sixteenBit
        ? mapReadbackBufferRgba16(m_impl->device, readbackBuffer, size,
                                  paddedRowBytes, error)
        : mapReadbackBuffer(m_impl->device, readbackBuffer, size,
                            paddedRowBytes, error);
    cleanup();
    if (result.isNull() || isCancelled()) {
        if (result.isNull()) return {};
        if (error) *error = QStringLiteral("Display transform cancelled");
        return {};
    }

    // RGBA16Float cannot represent every 16-bit alpha exactly. Presentation
    // transforms never alter alpha, so restore it verbatim from the source.
    if (sixteenBit) {
        const QImage original = source.convertToFormat(QImage::Format_RGBA64);
        auto converted = result.convertToFormat(QImage::Format_RGBA64);
        for (int y = 0; y < converted.height(); ++y) {
            const auto *sourceRow = reinterpret_cast<const QRgba64 *>(
                original.constScanLine(y));
            auto *targetRow = reinterpret_cast<QRgba64 *>(converted.scanLine(y));
            for (int x = 0; x < converted.width(); ++x) {
                const QRgba64 pixel = targetRow[x];
                targetRow[x] = QRgba64::fromRgba64(
                    pixel.red(), pixel.green(), pixel.blue(), sourceRow[x].alpha());
            }
        }
        result = std::move(converted);
    }
    if (result.format() != originalFormat) {
        result = result.convertToFormat(originalFormat);
    }
    result.setColorSpace({});
    result.setDevicePixelRatio(source.devicePixelRatio());
    result.setDotsPerMeterX(source.dotsPerMeterX());
    result.setDotsPerMeterY(source.dotsPerMeterY());
    return result;
#else
    Q_UNUSED(source)
    Q_UNUSED(lut)
    Q_UNUSED(cancelRequested)
    if (error) *error = QStringLiteral(
        "VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}

QImage WebGpuContext::resampleImageTiled(
    const QImage &source,
    const QSize &destinationSize,
    const ImageResampleMethod method,
    const std::atomic_bool *cancelRequested,
    QString *error)
{
    if (error) {
        error->clear();
    }
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    const auto isCancelled = [cancelRequested] {
        return cancelRequested
            && cancelRequested->load(std::memory_order_acquire);
    };
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (source.isNull() || source.depth() > 32 || destinationSize.isEmpty()) {
        if (error) {
            *error = source.depth() > 32
                ? QStringLiteral("Native tiled GPU resize currently supports 8-bit payloads; using the exact 16-bit CPU reference")
                : QStringLiteral("GPU resize input is empty");
        }
        return {};
    }
    if (method != ImageResampleMethod::NearestNeighbour
        && method != ImageResampleMethod::Bilinear) {
        if (error) {
            *error = QStringLiteral(
                "The requested high-quality resize filter uses the exact CPU reference");
        }
        return {};
    }
    if (isCancelled()) {
        if (error) *error = QStringLiteral("Image resize cancelled");
        return {};
    }
    if (source.size() == destinationSize) {
        return source.copy();
    }

    static constexpr char resampleShaderSource[] = R"WGSL(
// Tiled straight-component image resize. Each invocation computes the same
// global pixel-centre mapping as the CPU reference, then addresses a bounded
// source patch whose origin is supplied by the host. No filtering is performed
// in associated-alpha space: hidden RGB remains independent data.
struct ResampleParams {
    source_size: vec2<u32>,
    destination_size: vec2<u32>,
    source_origin: vec2<i32>,
    destination_origin: vec2<u32>,
    method: u32,
    _padding0: u32,
    _padding1: u32,
    _padding2: u32,
};

@group(0) @binding(0) var source_patch: texture_2d<f32>;
@group(0) @binding(1) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<uniform> params: ResampleParams;

fn nearest_index(destination: u32, source_extent: u32, destination_extent: u32) -> i32 {
    let numerator = (2u * destination + 1u) * source_extent;
    return i32(numerator / (2u * destination_extent));
}

fn linear_position(destination: u32, source_extent: u32, destination_extent: u32) -> vec2<f32> {
    let denominator = i32(2u * destination_extent);
    let numerator = i32((2u * destination + 1u) * source_extent)
        - i32(destination_extent);
    var base = numerator / denominator;
    var remainder = numerator % denominator;
    if (remainder < 0) {
        base = base - 1;
        remainder = remainder + denominator;
    }
    return vec2<f32>(f32(base), f32(remainder) / f32(denominator));
}

fn clamped_index(value: i32, extent: u32) -> i32 {
    return clamp(value, 0, i32(extent) - 1);
}

fn patch_load(global_source: vec2<i32>) -> vec4<f32> {
    return textureLoad(source_patch, global_source - params.source_origin, 0);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let output_dimensions = textureDimensions(output_tile);
    if (gid.x >= output_dimensions.x || gid.y >= output_dimensions.y) {
        return;
    }

    let destination = params.destination_origin + gid.xy;

    if (params.method == 0u) {
        let nearest = vec2<i32>(
            clamped_index(nearest_index(destination.x,
                                        params.source_size.x,
                                        params.destination_size.x),
                          params.source_size.x),
            clamped_index(nearest_index(destination.y,
                                        params.source_size.y,
                                        params.destination_size.y),
                          params.source_size.y));
        textureStore(output_tile, vec2<i32>(gid.xy), patch_load(nearest));
        return;
    }

    let source_x = linear_position(destination.x,
                                   params.source_size.x,
                                   params.destination_size.x);
    let source_y = linear_position(destination.y,
                                   params.source_size.y,
                                   params.destination_size.y);
    let base_x = i32(source_x.x);
    let base_y = i32(source_y.x);
    let x0 = clamped_index(base_x, params.source_size.x);
    let x1 = clamped_index(base_x + 1, params.source_size.x);
    let y0 = clamped_index(base_y, params.source_size.y);
    let y1 = clamped_index(base_y + 1, params.source_size.y);
    let fraction = vec2<f32>(source_x.y, source_y.y);
    let top = mix(patch_load(vec2<i32>(x0, y0)),
                  patch_load(vec2<i32>(x1, y0)), fraction.x);
    let bottom = mix(patch_load(vec2<i32>(x0, y1)),
                     patch_load(vec2<i32>(x1, y1)), fraction.x);
    textureStore(output_tile, vec2<i32>(gid.xy), mix(top, bottom, fraction.y));
}
)WGSL";

    if (!m_impl->resamplePipeline) {
        if (m_impl->resampleShader) {
            wgpuShaderModuleRelease(m_impl->resampleShader);
            m_impl->resampleShader = nullptr;
        }
        m_impl->resampleShader = createShaderModule(m_impl->device,
                                                    "PhotoLab tiled resize shader",
                                                    resampleShaderSource);
        if (m_impl->resampleShader) {
            WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView("PhotoLab tiled resize pipeline");
            descriptor.compute.module = m_impl->resampleShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->resamplePipeline = wgpuDeviceCreateComputePipeline(
                m_impl->device, &descriptor);
        }
        if (!m_impl->resamplePipeline) {
            if (m_impl->resampleShader) {
                wgpuShaderModuleRelease(m_impl->resampleShader);
                m_impl->resampleShader = nullptr;
            }
            if (error) *error = QStringLiteral("GPU resize pipeline creation failed");
            return {};
        }
    }

    const bool grayscale = source.format() == QImage::Format_Grayscale8;
    QImage output(destinationSize,
                  grayscale ? QImage::Format_Grayscale8
                            : QImage::Format_RGBA8888);
    if (output.isNull()) {
        if (error) *error = QStringLiteral("GPU resize output allocation failed");
        return {};
    }
    output.fill(0);

    constexpr int DestinationTileSize = 256;
    constexpr int MaximumSourcePatchExtent = 2048;
    const double ratioX = source.width()
        / static_cast<double>(destinationSize.width());
    const double ratioY = source.height()
        / static_cast<double>(destinationSize.height());
    const auto preferredDestinationExtent = [=](const double ratio) {
        if (!std::isfinite(ratio) || ratio <= 0.0) {
            return 1;
        }
        return std::clamp(
            static_cast<int>(std::floor((MaximumSourcePatchExtent - 2.0)
                                        / ratio)),
            1,
            DestinationTileSize);
    };
    const int preferredTileWidth = preferredDestinationExtent(ratioX);
    const int preferredTileHeight = preferredDestinationExtent(ratioY);

    struct AxisBounds {
        int first = 0;
        int last = -1;
    };
    const auto axisBounds = [method](const int sourceExtent,
                                     const int destinationExtent,
                                     const int destinationStart,
                                     const int destinationCount) {
        AxisBounds bounds;
        bounds.first = sourceExtent;
        bounds.last = -1;
        const int64_t denominator = int64_t(destinationExtent) * 2;
        for (int destination = destinationStart;
             destination < destinationStart + destinationCount;
             ++destination) {
            const int64_t doubledCentre = int64_t(destination) * 2 + 1;
            if (method == ImageResampleMethod::NearestNeighbour) {
                const int nearest = std::clamp(
                    static_cast<int>((doubledCentre * sourceExtent) / denominator),
                    0,
                    sourceExtent - 1);
                bounds.first = std::min(bounds.first, nearest);
                bounds.last = std::max(bounds.last, nearest);
            } else {
                const int64_t numerator = doubledCentre * sourceExtent
                    - destinationExtent;
                int64_t base = numerator / denominator;
                const int64_t remainder = numerator % denominator;
                if (remainder < 0) {
                    --base;
                }
                const int first = std::clamp(
                    static_cast<int>(base), 0, sourceExtent - 1);
                const int second = std::clamp(
                    static_cast<int>(base + 1), 0, sourceExtent - 1);
                bounds.first = std::min(bounds.first, std::min(first, second));
                bounds.last = std::max(bounds.last, std::max(first, second));
            }
        }
        return bounds;
    };

    struct alignas(16) ResampleParams {
        uint32_t sourceSize[2];
        uint32_t destinationSize[2];
        int32_t sourceOrigin[2];
        uint32_t destinationOrigin[2];
        uint32_t method;
        uint32_t padding[3];
    };
    static_assert(sizeof(ResampleParams) == 48);

    for (int destinationY = 0; destinationY < destinationSize.height();) {
        if (isCancelled()) {
            if (error) *error = QStringLiteral("Image resize cancelled");
            return {};
        }
        int tileHeight = std::min(preferredTileHeight,
                                  destinationSize.height() - destinationY);
        AxisBounds yBounds = axisBounds(source.height(),
                                        destinationSize.height(),
                                        destinationY,
                                        tileHeight);
        while (yBounds.last - yBounds.first + 1 > MaximumSourcePatchExtent
               && tileHeight > 1) {
            tileHeight = std::max(1, tileHeight / 2);
            yBounds = axisBounds(source.height(),
                                 destinationSize.height(),
                                 destinationY,
                                 tileHeight);
        }

        for (int destinationX = 0; destinationX < destinationSize.width();) {
            if (isCancelled()) {
                if (error) *error = QStringLiteral("Image resize cancelled");
                return {};
            }
            int tileWidth = std::min(preferredTileWidth,
                                     destinationSize.width() - destinationX);
            AxisBounds xBounds = axisBounds(source.width(),
                                            destinationSize.width(),
                                            destinationX,
                                            tileWidth);
            while (xBounds.last - xBounds.first + 1 > MaximumSourcePatchExtent
                   && tileWidth > 1) {
                tileWidth = std::max(1, tileWidth / 2);
                xBounds = axisBounds(source.width(),
                                     destinationSize.width(),
                                     destinationX,
                                     tileWidth);
            }

            const QRect sourceRect(xBounds.first,
                                   yBounds.first,
                                   xBounds.last - xBounds.first + 1,
                                   yBounds.last - yBounds.first + 1);
            QImage sourcePatch = source.copy(sourceRect);
            if (sourcePatch.isNull()) {
                if (error) *error = QStringLiteral("Could not materialise a bounded GPU resize source patch");
                return {};
            }
            if (grayscale) {
                const QImage grey = sourcePatch.convertToFormat(QImage::Format_Grayscale8);
                QImage packed(grey.size(), QImage::Format_RGBA8888);
                if (packed.isNull()) {
                    if (error) *error = QStringLiteral("Could not pack a grayscale GPU resize source patch");
                    return {};
                }
                for (int y = 0; y < grey.height(); ++y) {
                    const uchar *sourceRow = grey.constScanLine(y);
                    uchar *packedRow = packed.scanLine(y);
                    for (int x = 0; x < grey.width(); ++x) {
                        const uchar value = sourceRow[x];
                        packedRow[x * 4] = value;
                        packedRow[x * 4 + 1] = value;
                        packedRow[x * 4 + 2] = value;
                        packedRow[x * 4 + 3] = 255;
                    }
                }
                sourcePatch = std::move(packed);
            } else {
                sourcePatch = sourcePatch.convertToFormat(QImage::Format_RGBA8888);
            }

            const QSize outputTileSize(tileWidth, tileHeight);
            const uint32_t width = static_cast<uint32_t>(tileWidth);
            const uint32_t height = static_cast<uint32_t>(tileHeight);
            const uint32_t paddedRowBytes = alignedBytesPerRow(width);
            const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes)
                * height;

            WGPUTexture sourceTexture = nullptr;
            WGPUTexture outputTexture = nullptr;
            WGPUTextureView sourceView = nullptr;
            WGPUTextureView outputView = nullptr;
            WGPUBuffer paramsBuffer = nullptr;
            WGPUBuffer readbackBuffer = nullptr;
            WGPUBindGroupLayout layout = nullptr;
            WGPUBindGroup bindGroup = nullptr;
            WGPUCommandEncoder encoder = nullptr;
            WGPUComputePassEncoder pass = nullptr;
            WGPUCommandBuffer commandBuffer = nullptr;

            const auto cleanup = [&] {
                if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
                if (pass) wgpuComputePassEncoderRelease(pass);
                if (encoder) wgpuCommandEncoderRelease(encoder);
                if (bindGroup) wgpuBindGroupRelease(bindGroup);
                if (layout) wgpuBindGroupLayoutRelease(layout);
                if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
                if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
                if (outputView) wgpuTextureViewRelease(outputView);
                if (sourceView) wgpuTextureViewRelease(sourceView);
                if (outputTexture) wgpuTextureRelease(outputTexture);
                if (sourceTexture) wgpuTextureRelease(sourceTexture);
            };
            const auto failTile = [&](const QString &message) {
                if (error && error->isEmpty()) {
                    *error = message;
                }
                cleanup();
            };

            sourceTexture = uploadTexture(m_impl->device,
                                          m_impl->queue,
                                          sourcePatch,
                                          "PhotoLab resize source patch",
                                          false,
                                          error);
            sourceView = sourceTexture
                ? wgpuTextureCreateView(sourceTexture, nullptr) : nullptr;
            outputTexture = createWorkingTexture(m_impl->device,
                                                 outputTileSize,
                                                 "PhotoLab resize output tile",
                                                 error);
            outputView = outputTexture
                ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
            if (!sourceTexture || !sourceView || !outputTexture || !outputView) {
                cleanup();
                return {};
            }

            ResampleParams params {};
            params.sourceSize[0] = static_cast<uint32_t>(source.width());
            params.sourceSize[1] = static_cast<uint32_t>(source.height());
            params.destinationSize[0] = static_cast<uint32_t>(destinationSize.width());
            params.destinationSize[1] = static_cast<uint32_t>(destinationSize.height());
            params.sourceOrigin[0] = sourceRect.x();
            params.sourceOrigin[1] = sourceRect.y();
            params.destinationOrigin[0] = static_cast<uint32_t>(destinationX);
            params.destinationOrigin[1] = static_cast<uint32_t>(destinationY);
            params.method = method == ImageResampleMethod::NearestNeighbour ? 0u : 1u;

            WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
            paramsDescriptor.label = stringView("PhotoLab resize parameters");
            paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            paramsDescriptor.size = sizeof(params);
            paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
            WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
            readbackDescriptor.label = stringView("PhotoLab resize readback");
            readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
            readbackDescriptor.size = readbackSize;
            readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
            if (!paramsBuffer || !readbackBuffer) {
                failTile(QStringLiteral("GPU resize buffer allocation failed"));
                return {};
            }
            wgpuQueueWriteBuffer(m_impl->queue,
                                 paramsBuffer,
                                 0,
                                 &params,
                                 sizeof(params));

            layout = wgpuComputePipelineGetBindGroupLayout(
                m_impl->resamplePipeline, 0);
            WGPUBindGroupEntry entries[3] = {
                WGPU_BIND_GROUP_ENTRY_INIT,
                WGPU_BIND_GROUP_ENTRY_INIT,
                WGPU_BIND_GROUP_ENTRY_INIT
            };
            entries[0].binding = 0;
            entries[0].textureView = sourceView;
            entries[1].binding = 1;
            entries[1].textureView = outputView;
            entries[2].binding = 2;
            entries[2].buffer = paramsBuffer;
            entries[2].size = sizeof(params);
            WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bindDescriptor.label = stringView("PhotoLab resize bind group");
            bindDescriptor.layout = layout;
            bindDescriptor.entryCount = 3;
            bindDescriptor.entries = entries;
            bindGroup = layout
                ? wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor)
                : nullptr;
            if (!layout || !bindGroup) {
                failTile(QStringLiteral("GPU resize bind group creation failed"));
                return {};
            }

            WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
            encoderDescriptor.label = stringView("PhotoLab resize command encoder");
            encoder = wgpuDeviceCreateCommandEncoder(m_impl->device,
                                                     &encoderDescriptor);
            WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
            passDescriptor.label = stringView("PhotoLab resize compute pass");
            pass = encoder
                ? wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor)
                : nullptr;
            if (!encoder || !pass) {
                failTile(QStringLiteral("GPU resize command encoding failed"));
                return {};
            }
            wgpuComputePassEncoderSetPipeline(pass, m_impl->resamplePipeline);
            wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass, (width + 7) / 8, (height + 7) / 8, 1);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            pass = nullptr;

            WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            copySource.texture = outputTexture;
            WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
            copyDestination.buffer = readbackBuffer;
            copyDestination.layout.bytesPerRow = paddedRowBytes;
            copyDestination.layout.rowsPerImage = height;
            const WGPUExtent3D extent {width, height, 1};
            wgpuCommandEncoderCopyTextureToBuffer(encoder,
                                                  &copySource,
                                                  &copyDestination,
                                                  &extent);
            WGPUCommandBufferDescriptor commandDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
            commandDescriptor.label = stringView("PhotoLab resize tile commands");
            commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
            wgpuCommandEncoderRelease(encoder);
            encoder = nullptr;
            if (!commandBuffer) {
                failTile(QStringLiteral("GPU resize command buffer creation failed"));
                return {};
            }
            wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
            QImage tile = mapReadbackBuffer(m_impl->device,
                                            readbackBuffer,
                                            outputTileSize,
                                            paddedRowBytes,
                                            error);
            cleanup();
            if (tile.isNull()) {
                return {};
            }
            if (isCancelled()) {
                if (error) *error = QStringLiteral("Image resize cancelled");
                return {};
            }

            if (grayscale) {
                for (int y = 0; y < tileHeight; ++y) {
                    const uchar *tileRow = tile.constScanLine(y);
                    uchar *destinationRow = output.scanLine(destinationY + y)
                        + destinationX;
                    for (int x = 0; x < tileWidth; ++x) {
                        destinationRow[x] = tileRow[x * 4];
                    }
                }
            } else {
                for (int y = 0; y < tileHeight; ++y) {
                    std::memcpy(output.scanLine(destinationY + y)
                                    + destinationX * 4,
                                tile.constScanLine(y),
                                static_cast<size_t>(tileWidth) * 4);
                }
            }
            destinationX += tileWidth;
        }
        destinationY += tileHeight;
    }

    output.setColorSpace(source.colorSpace());
    output.setDevicePixelRatio(source.devicePixelRatio());
    output.setDotsPerMeterX(source.dotsPerMeterX());
    output.setDotsPerMeterY(source.dotsPerMeterY());
    return output;
#else
    Q_UNUSED(source)
    Q_UNUSED(destinationSize)
    Q_UNUSED(method)
    Q_UNUSED(cancelRequested)
    if (error) *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}


QImage WebGpuContext::stampBrushTile(const QImage &source,
                                     const QPoint &tileOrigin,
                                     const QVector<QPointF> &stampPoints,
                                     const double radius,
                                     const double hardness,
                                     const double opacity,
                                     const QColor &colour,
                                     const bool erasing,
                                     const quint64 residencyKey,
                                     const quint64 sourceRevision,
                                     const QImage &selectionCoverage,
                                     QString *error)
{
    if (error) error->clear();
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (source.isNull() || stampPoints.isEmpty() || radius <= 0.0) {
        if (error) *error = QStringLiteral("Brush tile input is empty");
        return {};
    }

    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    const QSize size = rgba.size();
    QImage selection = selectionCoverage;
    if (selection.isNull() || selection.size() != size) {
        selection = QImage(size, QImage::Format_RGBA8888);
        selection.fill(QColor(255, 255, 255, 255));
    } else {
        selection = selection.convertToFormat(QImage::Format_RGBA8888);
    }
    const uint32_t width = static_cast<uint32_t>(size.width());
    const uint32_t height = static_cast<uint32_t>(size.height());
    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes) * height;

    struct alignas(16) BrushParams {
        float tileOrigin[2];
        float radius;
        float hardness;
        float opacity;
        uint32_t erasing;
        uint32_t pointCount;
        uint32_t padding;
        float colour[4];
    };
    static_assert(sizeof(BrushParams) == 48);
    BrushParams params {};
    params.tileOrigin[0] = static_cast<float>(tileOrigin.x());
    params.tileOrigin[1] = static_cast<float>(tileOrigin.y());
    params.radius = static_cast<float>(std::max(0.5, radius));
    params.hardness = static_cast<float>(std::clamp(hardness, 0.0, 0.9999));
    params.opacity = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    params.erasing = erasing ? 1u : 0u;
    params.pointCount = static_cast<uint32_t>(stampPoints.size());
    params.colour[0] = static_cast<float>(colour.redF());
    params.colour[1] = static_cast<float>(colour.greenF());
    params.colour[2] = static_cast<float>(colour.blueF());
    params.colour[3] = static_cast<float>(colour.alphaF());

    QByteArray pointBytes(static_cast<qsizetype>(stampPoints.size() * 2 * sizeof(float)), '\0');
    auto *pointData = reinterpret_cast<float *>(pointBytes.data());
    for (qsizetype index = 0; index < stampPoints.size(); ++index) {
        pointData[index * 2] = static_cast<float>(stampPoints.at(index).x());
        pointData[index * 2 + 1] = static_cast<float>(stampPoints.at(index).y());
    }

    static constexpr char brushShaderSource[] = R"WGSL(
// Canonical dirty-tile brush kernel. A whole discrete stamp sequence is applied
// to one immutable straight-RGBA source tile in a single dispatch. Selection
// coverage is applied once to the completed stroke so CPU/GPU paths agree even
// where multiple stamps overlap. Erasing always preserves hidden RGB.
struct BrushParams {
    tile_origin: vec2<f32>,
    radius: f32,
    hardness: f32,
    opacity: f32,
    erasing: u32,
    point_count: u32,
    _padding: u32,
    colour: vec4<f32>,
};

@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(2) var<storage, read> points: array<vec2<f32>>;
@group(0) @binding(3) var<uniform> params: BrushParams;
@group(0) @binding(4) var selection_tile: texture_2d<f32>;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }

    let local = vec2<i32>(gid.xy);
    let layer_point = params.tile_origin + vec2<f32>(gid.xy) + vec2<f32>(0.5);
    let source = textureLoad(source_tile, local, 0);
    var edited = source;
    for (var index: u32 = 0u; index < params.point_count; index = index + 1u) {
        let normalised_distance = distance(layer_point, points[index]) / max(params.radius, 0.5);
        let coverage = 1.0 - smoothstep(min(params.hardness, 0.9999), 1.0, normalised_distance);
        let amount = clamp(coverage * params.opacity * params.colour.a, 0.0, 1.0);
        if (params.erasing != 0u) {
            edited = vec4<f32>(edited.rgb, edited.a * (1.0 - amount));
        } else {
            let output_alpha = amount + edited.a * (1.0 - amount);
            var output_rgb = edited.rgb;
            if (output_alpha > 0.000001) {
                output_rgb = (params.colour.rgb * amount
                    + edited.rgb * edited.a * (1.0 - amount)) / output_alpha;
            }
            edited = vec4<f32>(output_rgb, output_alpha);
        }
    }

    let selection_amount = clamp(textureLoad(selection_tile, local, 0).r, 0.0, 1.0);
    let output_alpha = source.a + (edited.a - source.a) * selection_amount;
    var output_rgb = source.rgb;
    if (output_alpha > 0.000001) {
        let output_premultiplied = source.rgb * source.a * (1.0 - selection_amount)
            + edited.rgb * edited.a * selection_amount;
        output_rgb = output_premultiplied / output_alpha;
    }
    textureStore(output_tile, local, vec4<f32>(output_rgb, output_alpha));
}
)WGSL";

    if (!m_impl->brushPipeline) {
        m_impl->brushShader = createShaderModule(m_impl->device,
                                                  "PhotoLab tiled brush shader",
                                                  brushShaderSource);
        if (m_impl->brushShader) {
            WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView("PhotoLab tiled brush pipeline");
            descriptor.compute.module = m_impl->brushShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->brushPipeline = wgpuDeviceCreateComputePipeline(m_impl->device, &descriptor);
        }
        if (!m_impl->brushPipeline) {
            if (error) *error = QStringLiteral("GPU brush pipeline creation failed");
            return {};
        }
    }

    WGPUTexture sourceTexture = nullptr;
    WGPUTexture selectionTexture = nullptr;
    WGPUTexture outputTexture = nullptr;
    WGPUTextureView sourceView = nullptr;
    WGPUTextureView selectionView = nullptr;
    WGPUTextureView outputView = nullptr;
    WGPUBuffer pointsBuffer = nullptr;
    WGPUBuffer paramsBuffer = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;
    bool sourceBorrowed = false;

    auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
        if (pointsBuffer) wgpuBufferRelease(pointsBuffer);
        if (outputView) wgpuTextureViewRelease(outputView);
        if (selectionView) wgpuTextureViewRelease(selectionView);
        if (!sourceBorrowed && sourceView) wgpuTextureViewRelease(sourceView);
        if (outputTexture) wgpuTextureRelease(outputTexture);
        if (selectionTexture) wgpuTextureRelease(selectionTexture);
        if (!sourceBorrowed && sourceTexture) wgpuTextureRelease(sourceTexture);
    };

    // Resident compositing tiles are stored premultiplied. The brush kernel now
    // consumes straight RGBA to preserve hidden colour, so upload this immutable
    // source explicitly rather than borrowing a differently encoded texture.
    Q_UNUSED(residencyKey);
    Q_UNUSED(sourceRevision);
    sourceTexture = uploadTexture(m_impl->device, m_impl->queue, rgba,
                                  "PhotoLab straight brush source tile", false, error);
    sourceView = sourceTexture ? wgpuTextureCreateView(sourceTexture, nullptr) : nullptr;
    selectionTexture = uploadTexture(m_impl->device, m_impl->queue, selection,
                                     "PhotoLab brush selection tile", false, error);
    selectionView = selectionTexture
        ? wgpuTextureCreateView(selectionTexture, nullptr) : nullptr;
    outputTexture = createWorkingTexture(m_impl->device, size,
                                         "PhotoLab brush output tile", error);
    outputView = outputTexture ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
    if (!sourceTexture || !sourceView || !selectionTexture || !selectionView
        || !outputTexture || !outputView) {
        cleanup();
        return {};
    }

    WGPUBufferDescriptor pointsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    pointsDescriptor.label = stringView("PhotoLab brush stamp points");
    pointsDescriptor.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    pointsDescriptor.size = static_cast<uint64_t>(pointBytes.size());
    pointsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &pointsDescriptor);
    WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramsDescriptor.label = stringView("PhotoLab brush parameters");
    paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramsDescriptor.size = sizeof(params);
    paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab brush readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = readbackSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!pointsBuffer || !paramsBuffer || !readbackBuffer) {
        if (error) *error = QStringLiteral("GPU brush buffer allocation failed");
        cleanup();
        return {};
    }
    wgpuQueueWriteBuffer(m_impl->queue, pointsBuffer, 0,
                         pointBytes.constData(), static_cast<size_t>(pointBytes.size()));
    wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

    layout = wgpuComputePipelineGetBindGroupLayout(m_impl->brushPipeline, 0);
    WGPUBindGroupEntry entries[5] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = sourceView;
    entries[1].binding = 1; entries[1].textureView = outputView;
    entries[2].binding = 2; entries[2].buffer = pointsBuffer; entries[2].size = pointBytes.size();
    entries[3].binding = 3; entries[3].buffer = paramsBuffer; entries[3].size = sizeof(params);
    entries[4].binding = 4; entries[4].textureView = selectionView;
    WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindDescriptor.label = stringView("PhotoLab brush bind group");
    bindDescriptor.layout = layout;
    bindDescriptor.entryCount = 5;
    bindDescriptor.entries = entries;
    bindGroup = wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor);
    if (!layout || !bindGroup) {
        if (error) *error = QStringLiteral("GPU brush bind group creation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("PhotoLab brush command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab brush compute pass");
    pass = encoder ? wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor) : nullptr;
    if (!encoder || !pass) {
        if (error) *error = QStringLiteral("GPU brush command encoding failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, m_impl->brushPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = outputTexture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource, &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab brush tile commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral("GPU brush command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage result = mapReadbackBuffer(m_impl->device, readbackBuffer, size,
                                      paddedRowBytes, error);
    cleanup();
    if (!result.isNull()) result.setColorSpace(source.colorSpace());
    return result;
#else
    Q_UNUSED(source) Q_UNUSED(tileOrigin) Q_UNUSED(stampPoints) Q_UNUSED(radius)
    Q_UNUSED(hardness) Q_UNUSED(opacity) Q_UNUSED(colour) Q_UNUSED(erasing)
    Q_UNUSED(residencyKey) Q_UNUSED(sourceRevision) Q_UNUSED(selectionCoverage)
    if (error) *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}


QImage WebGpuContext::applyFillTile(const QImage &source,
                                    const QImage &coverage,
                                    const FillTarget target,
                                    const int componentIndex,
                                    const QColor &colour,
                                    const bool preserveTransparency,
                                    QString *error)
{
    if (error) error->clear();
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (source.isNull() || coverage.isNull() || source.size() != coverage.size()) {
        if (error) *error = QStringLiteral("Fill tile input is invalid");
        return {};
    }
    if (target == FillTarget::ComponentChannel
        && (componentIndex < 0 || componentIndex > 3)) {
        if (error) *error = QStringLiteral("Fill tile component target is invalid");
        return {};
    }

    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    const QImage coverageRgba = coverage.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull() || coverageRgba.isNull()) {
        if (error) *error = QStringLiteral("Fill tile images could not be converted");
        return {};
    }
    const QSize size = rgba.size();
    const uint32_t width = static_cast<uint32_t>(size.width());
    const uint32_t height = static_cast<uint32_t>(size.height());
    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes) * height;

    struct alignas(16) FillParams {
        float colour[4];
        uint32_t targetMode;
        int32_t componentIndex;
        uint32_t preserveTransparency;
        uint32_t padding;
    };
    static_assert(sizeof(FillParams) == 32);
    FillParams params {};
    const float scalar = static_cast<float>(qGray(colour.rgb()) / 255.0);
    if (target == FillTarget::RasterPixels) {
        params.colour[0] = static_cast<float>(colour.redF());
        params.colour[1] = static_cast<float>(colour.greenF());
        params.colour[2] = static_cast<float>(colour.blueF());
        params.colour[3] = static_cast<float>(colour.alphaF());
    } else {
        params.colour[0] = scalar;
        params.colour[1] = scalar;
        params.colour[2] = scalar;
        params.colour[3] = 1.0f;
    }
    params.targetMode = static_cast<uint32_t>(target);
    params.componentIndex = componentIndex;
    params.preserveTransparency = preserveTransparency ? 1u : 0u;

    static constexpr char fillShaderSource[] = R"WGSL(
// Coverage-driven straight-RGBA Fill kernel. Region matching and contiguous
// discovery are deterministic CPU work; this native path accelerates bounded
// dirty-tile application while preserving hidden RGB and exact channel/mask
// semantics. Partial raster coverage blends in premultiplied space while
// the stored output remains straight RGBA.
struct FillParams {
    colour: vec4<f32>,
    target_mode: u32,
    component_index: i32,
    preserve_transparency: u32,
    _padding: u32,
};

@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var coverage_tile: texture_2d<f32>;
@group(0) @binding(2) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: FillParams;

fn quantise(value: f32) -> f32 {
    return floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

fn quantise4(value: vec4<f32>) -> vec4<f32> {
    return vec4<f32>(quantise(value.r), quantise(value.g),
                     quantise(value.b), quantise(value.a));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }
    let local = vec2<i32>(gid.xy);
    let source_value = textureLoad(source_tile, local, 0);
    let amount = clamp(textureLoad(coverage_tile, local, 0).r, 0.0, 1.0);
    var edited = source_value;

    if (params.target_mode == 0u) {
        if (params.preserve_transparency != 0u) {
            let output_rgb = mix(source_value.rgb, params.colour.rgb, amount);
            edited = vec4<f32>(output_rgb, source_value.a);
        } else {
            let output_alpha = mix(source_value.a, params.colour.a, amount);
            var output_rgb = source_value.rgb;
            if (output_alpha > 0.000000001) {
                output_rgb = (source_value.rgb * source_value.a * (1.0 - amount)
                              + params.colour.rgb * params.colour.a * amount)
                    / output_alpha;
            }
            edited = vec4<f32>(output_rgb, output_alpha);
        }
    } else if (params.target_mode == 1u) {
        let output_rgb = mix(source_value.rgb, vec3<f32>(params.colour.r), amount);
        edited = vec4<f32>(output_rgb, source_value.a);
    } else if (params.target_mode == 2u) {
        if (params.component_index == 0) {
            edited.r = mix(source_value.r, params.colour.r, amount);
        } else if (params.component_index == 1) {
            edited.g = mix(source_value.g, params.colour.r, amount);
        } else if (params.component_index == 2) {
            edited.b = mix(source_value.b, params.colour.r, amount);
        } else if (params.component_index == 3) {
            edited.a = mix(source_value.a, params.colour.r, amount);
        }
    } else {
        let value = mix(source_value.r, params.colour.r, amount);
        edited = vec4<f32>(value, value, value, 1.0);
    }
    textureStore(output_tile, local, quantise4(edited));
}
)WGSL";

    if (!m_impl->fillPipeline) {
        m_impl->fillShader = createShaderModule(m_impl->device,
                                                 "PhotoLab tiled fill shader",
                                                 fillShaderSource,
                                                 error);
        if (m_impl->fillShader) {
            WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView("PhotoLab tiled fill pipeline");
            descriptor.compute.module = m_impl->fillShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->fillPipeline = wgpuDeviceCreateComputePipeline(m_impl->device,
                                                                    &descriptor);
        }
        if (!m_impl->fillPipeline) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("GPU Fill pipeline creation failed");
            }
            return {};
        }
    }

    WGPUTexture sourceTexture = nullptr;
    WGPUTexture coverageTexture = nullptr;
    WGPUTexture outputTexture = nullptr;
    WGPUTextureView sourceView = nullptr;
    WGPUTextureView coverageView = nullptr;
    WGPUTextureView outputView = nullptr;
    WGPUBuffer paramsBuffer = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;

    auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
        if (outputView) wgpuTextureViewRelease(outputView);
        if (coverageView) wgpuTextureViewRelease(coverageView);
        if (sourceView) wgpuTextureViewRelease(sourceView);
        if (outputTexture) wgpuTextureRelease(outputTexture);
        if (coverageTexture) wgpuTextureRelease(coverageTexture);
        if (sourceTexture) wgpuTextureRelease(sourceTexture);
    };

    sourceTexture = uploadTexture(m_impl->device, m_impl->queue, rgba,
                                  "PhotoLab straight Fill source tile", false, error);
    coverageTexture = uploadTexture(m_impl->device, m_impl->queue, coverageRgba,
                                    "PhotoLab Fill coverage tile", false, error);
    outputTexture = createWorkingTexture(m_impl->device, size,
                                         "PhotoLab Fill output tile", error);
    sourceView = sourceTexture ? wgpuTextureCreateView(sourceTexture, nullptr) : nullptr;
    coverageView = coverageTexture ? wgpuTextureCreateView(coverageTexture, nullptr) : nullptr;
    outputView = outputTexture ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
    if (!sourceTexture || !coverageTexture || !outputTexture
        || !sourceView || !coverageView || !outputView) {
        cleanup();
        return {};
    }

    WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramsDescriptor.label = stringView("PhotoLab Fill parameters");
    paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramsDescriptor.size = sizeof(params);
    paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab Fill readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = readbackSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!paramsBuffer || !readbackBuffer) {
        if (error) *error = QStringLiteral("GPU Fill buffer allocation failed");
        cleanup();
        return {};
    }
    wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

    layout = wgpuComputePipelineGetBindGroupLayout(m_impl->fillPipeline, 0);
    WGPUBindGroupEntry entries[4] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = sourceView;
    entries[1].binding = 1; entries[1].textureView = coverageView;
    entries[2].binding = 2; entries[2].textureView = outputView;
    entries[3].binding = 3; entries[3].buffer = paramsBuffer; entries[3].size = sizeof(params);
    WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindDescriptor.label = stringView("PhotoLab Fill bind group");
    bindDescriptor.layout = layout;
    bindDescriptor.entryCount = 4;
    bindDescriptor.entries = entries;
    bindGroup = wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor);
    if (!layout || !bindGroup) {
        if (error) *error = QStringLiteral("GPU Fill bind group creation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("PhotoLab Fill command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab Fill compute pass");
    pass = encoder ? wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor) : nullptr;
    if (!encoder || !pass) {
        if (error) *error = QStringLiteral("GPU Fill command encoding failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, m_impl->fillPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = outputTexture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource, &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab Fill tile commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral("GPU Fill command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage result = mapReadbackBuffer(m_impl->device, readbackBuffer, size,
                                      paddedRowBytes, error);
    cleanup();
    if (!result.isNull()) {
        result.setColorSpace(source.colorSpace());
        result.setDevicePixelRatio(source.devicePixelRatio());
        result.setDotsPerMeterX(source.dotsPerMeterX());
        result.setDotsPerMeterY(source.dotsPerMeterY());
    }
    return result;
#else
    Q_UNUSED(source)
    Q_UNUSED(coverage)
    Q_UNUSED(target)
    Q_UNUSED(componentIndex)
    Q_UNUSED(colour)
    Q_UNUSED(preserveTransparency)
    if (error) *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}


QImage WebGpuContext::applyGradientTile(const QImage &source,
                                        const QImage &coverage,
                                        const QPoint &tileOrigin,
                                        const FillTarget target,
                                        const int componentIndex,
                                        const QPointF &start,
                                        const QPointF &end,
                                        const RasterGradientType type,
                                        const QColor &startColour,
                                        const QColor &endColour,
                                        const bool reverse,
                                        QString *error)
{
    if (error) error->clear();
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (source.isNull() || coverage.isNull() || source.size() != coverage.size()) {
        if (error) *error = QStringLiteral("Gradient tile input is invalid");
        return {};
    }
    if (target == FillTarget::ComponentChannel
        && (componentIndex < 0 || componentIndex > 3)) {
        if (error) *error = QStringLiteral("Gradient tile component target is invalid");
        return {};
    }

    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    const QImage coverageRgba = coverage.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull() || coverageRgba.isNull()) {
        if (error) *error = QStringLiteral("Gradient tile images could not be converted");
        return {};
    }
    const QSize size = rgba.size();
    const uint32_t width = static_cast<uint32_t>(size.width());
    const uint32_t height = static_cast<uint32_t>(size.height());
    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes) * height;

    struct alignas(16) GradientParams {
        float startColour[4];
        float endColour[4];
        float geometry[4];
        float tileOrigin[2];
        uint32_t targetMode;
        int32_t componentIndex;
        uint32_t gradientType;
        uint32_t reverse;
        uint32_t padding[2];
    };
    static_assert(sizeof(GradientParams) == 80);
    GradientParams params {};
    const QColor colours[] {startColour, endColour};
    float *outputs[] {params.startColour, params.endColour};
    for (int index = 0; index < 2; ++index) {
        const float scalar = static_cast<float>(qGray(colours[index].rgb()) / 255.0);
        if (target == FillTarget::RasterPixels) {
            outputs[index][0] = static_cast<float>(colours[index].redF());
            outputs[index][1] = static_cast<float>(colours[index].greenF());
            outputs[index][2] = static_cast<float>(colours[index].blueF());
            outputs[index][3] = static_cast<float>(colours[index].alphaF());
        } else {
            outputs[index][0] = scalar;
            outputs[index][1] = scalar;
            outputs[index][2] = scalar;
            outputs[index][3] = 1.0f;
        }
    }
    params.geometry[0] = static_cast<float>(start.x());
    params.geometry[1] = static_cast<float>(start.y());
    params.geometry[2] = static_cast<float>(end.x());
    params.geometry[3] = static_cast<float>(end.y());
    params.tileOrigin[0] = static_cast<float>(tileOrigin.x());
    params.tileOrigin[1] = static_cast<float>(tileOrigin.y());
    params.targetMode = static_cast<uint32_t>(target);
    params.componentIndex = componentIndex;
    params.gradientType = static_cast<uint32_t>(type);
    params.reverse = reverse ? 1u : 0u;

    static constexpr char gradientShaderSource[] = R"WGSL(
// Selection-aware straight-RGBA raster Gradient kernel. Geometry is expressed
// in target-local pixels so adjacent 256x256 tiles evaluate exactly the same
// continuous gradient. Stored pixels remain straight RGBA; feathered raster
// coverage blends in premultiplied space to avoid transparent dark fringes.
struct GradientParams {
    start_colour: vec4<f32>,
    end_colour: vec4<f32>,
    geometry: vec4<f32>,
    tile_origin: vec2<f32>,
    target_mode: u32,
    component_index: i32,
    gradient_type: u32,
    reverse: u32,
    _padding: vec2<u32>,
};

@group(0) @binding(0) var source_tile: texture_2d<f32>;
@group(0) @binding(1) var coverage_tile: texture_2d<f32>;
@group(0) @binding(2) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<uniform> params: GradientParams;

fn quantise(value: f32) -> f32 {
    return floor(clamp(value, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
}

fn quantise4(value: vec4<f32>) -> vec4<f32> {
    return vec4<f32>(quantise(value.r), quantise(value.g),
                     quantise(value.b), quantise(value.a));
}

fn gradient_amount(point: vec2<f32>) -> f32 {
    let start = params.geometry.xy;
    let finish = params.geometry.zw;
    let direction = finish - start;
    let length_squared = dot(direction, direction);
    let length_value = sqrt(max(length_squared, 0.0));
    var amount = 1.0;
    if (length_value > 0.000000001) {
        let relative = point - start;
        if (params.gradient_type == 0u) {
            amount = dot(relative, direction) / length_squared;
        } else if (params.gradient_type == 1u) {
            amount = length(relative) / length_value;
        } else if (params.gradient_type == 2u) {
            let two_pi = 6.28318530717958647692;
            let base = atan2(direction.y, direction.x);
            let angle = atan2(relative.y, relative.x);
            amount = (angle - base) / two_pi;
            amount = amount - floor(amount);
        } else if (params.gradient_type == 3u) {
            amount = abs(dot(relative, direction) / length_squared);
        } else {
            let axis = direction / length_value;
            let perpendicular = vec2<f32>(-axis.y, axis.x);
            amount = (abs(dot(relative, axis))
                      + abs(dot(relative, perpendicular))) / length_value;
        }
    }
    amount = clamp(amount, 0.0, 1.0);
    if (params.reverse != 0u) {
        amount = 1.0 - amount;
    }
    return amount;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }
    let local = vec2<i32>(gid.xy);
    let source_value = textureLoad(source_tile, local, 0);
    let coverage = clamp(textureLoad(coverage_tile, local, 0).r, 0.0, 1.0);
    let point = params.tile_origin + vec2<f32>(f32(gid.x) + 0.5,
                                               f32(gid.y) + 0.5);
    let gradient_colour = quantise4(mix(params.start_colour,
                                        params.end_colour,
                                        gradient_amount(point)));
    var edited = source_value;

    if (params.target_mode == 0u) {
        let mathematical_alpha = mix(source_value.a, gradient_colour.a, coverage);
        let stored_alpha = quantise(mathematical_alpha);
        var output_rgb = source_value.rgb;
        if (stored_alpha > 0.0 && mathematical_alpha > 0.000000001) {
            output_rgb = (source_value.rgb * source_value.a * (1.0 - coverage)
                          + gradient_colour.rgb * gradient_colour.a * coverage)
                / mathematical_alpha;
        }
        edited = vec4<f32>(output_rgb, stored_alpha);
    } else {
        let scalar = dot(gradient_colour.rgb,
                         vec3<f32>(0.299, 0.587, 0.114));
        if (params.target_mode == 1u) {
            let output_rgb = mix(source_value.rgb, vec3<f32>(scalar), coverage);
            edited = vec4<f32>(output_rgb, source_value.a);
        } else if (params.target_mode == 2u) {
            if (params.component_index == 0) {
                edited.r = mix(source_value.r, scalar, coverage);
            } else if (params.component_index == 1) {
                edited.g = mix(source_value.g, scalar, coverage);
            } else if (params.component_index == 2) {
                edited.b = mix(source_value.b, scalar, coverage);
            } else if (params.component_index == 3) {
                edited.a = mix(source_value.a, scalar, coverage);
            }
        } else {
            let value = mix(source_value.r, scalar, coverage);
            edited = vec4<f32>(value, value, value, 1.0);
        }
    }
    textureStore(output_tile, local, quantise4(edited));
}
)WGSL";

    if (!m_impl->gradientPipeline) {
        m_impl->gradientShader = createShaderModule(m_impl->device,
                                                     "PhotoLab tiled gradient shader",
                                                     gradientShaderSource,
                                                     error);
        if (m_impl->gradientShader) {
            WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView("PhotoLab tiled gradient pipeline");
            descriptor.compute.module = m_impl->gradientShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->gradientPipeline = wgpuDeviceCreateComputePipeline(m_impl->device,
                                                                        &descriptor);
        }
        if (!m_impl->gradientPipeline) {
            if (error && error->isEmpty()) *error = QStringLiteral("GPU Gradient pipeline creation failed");
            return {};
        }
    }

    WGPUTexture sourceTexture = nullptr;
    WGPUTexture coverageTexture = nullptr;
    WGPUTexture outputTexture = nullptr;
    WGPUTextureView sourceView = nullptr;
    WGPUTextureView coverageView = nullptr;
    WGPUTextureView outputView = nullptr;
    WGPUBuffer paramsBuffer = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;
    auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
        if (outputView) wgpuTextureViewRelease(outputView);
        if (coverageView) wgpuTextureViewRelease(coverageView);
        if (sourceView) wgpuTextureViewRelease(sourceView);
        if (outputTexture) wgpuTextureRelease(outputTexture);
        if (coverageTexture) wgpuTextureRelease(coverageTexture);
        if (sourceTexture) wgpuTextureRelease(sourceTexture);
    };

    sourceTexture = uploadTexture(m_impl->device, m_impl->queue, rgba,
                                  "PhotoLab Gradient source tile", false, error);
    coverageTexture = uploadTexture(m_impl->device, m_impl->queue, coverageRgba,
                                    "PhotoLab Gradient coverage tile", false, error);
    outputTexture = createWorkingTexture(m_impl->device, size,
                                         "PhotoLab Gradient output tile", error);
    sourceView = sourceTexture ? wgpuTextureCreateView(sourceTexture, nullptr) : nullptr;
    coverageView = coverageTexture ? wgpuTextureCreateView(coverageTexture, nullptr) : nullptr;
    outputView = outputTexture ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
    if (!sourceTexture || !coverageTexture || !outputTexture
        || !sourceView || !coverageView || !outputView) {
        cleanup();
        return {};
    }

    WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramsDescriptor.label = stringView("PhotoLab Gradient parameters");
    paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramsDescriptor.size = sizeof(params);
    paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab Gradient readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = readbackSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!paramsBuffer || !readbackBuffer) {
        if (error) *error = QStringLiteral("GPU Gradient buffer allocation failed");
        cleanup();
        return {};
    }
    wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

    layout = wgpuComputePipelineGetBindGroupLayout(m_impl->gradientPipeline, 0);
    WGPUBindGroupEntry entries[4] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = sourceView;
    entries[1].binding = 1; entries[1].textureView = coverageView;
    entries[2].binding = 2; entries[2].textureView = outputView;
    entries[3].binding = 3; entries[3].buffer = paramsBuffer; entries[3].size = sizeof(params);
    WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindDescriptor.label = stringView("PhotoLab Gradient bind group");
    bindDescriptor.layout = layout;
    bindDescriptor.entryCount = 4;
    bindDescriptor.entries = entries;
    bindGroup = wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor);
    if (!layout || !bindGroup) {
        if (error) *error = QStringLiteral("GPU Gradient bind group creation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("PhotoLab Gradient command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab Gradient compute pass");
    pass = encoder ? wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor) : nullptr;
    if (!encoder || !pass) {
        if (error) *error = QStringLiteral("GPU Gradient command encoding failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, m_impl->gradientPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = outputTexture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource, &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab Gradient tile commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral("GPU Gradient command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage result = mapReadbackBuffer(m_impl->device, readbackBuffer, size,
                                      paddedRowBytes, error);
    cleanup();
    if (!result.isNull()) {
        result.setColorSpace(source.colorSpace());
        result.setDevicePixelRatio(source.devicePixelRatio());
        result.setDotsPerMeterX(source.dotsPerMeterX());
        result.setDotsPerMeterY(source.dotsPerMeterY());
    }
    return result;
#else
    Q_UNUSED(source)
    Q_UNUSED(coverage)
    Q_UNUSED(tileOrigin)
    Q_UNUSED(target)
    Q_UNUSED(componentIndex)
    Q_UNUSED(start)
    Q_UNUSED(end)
    Q_UNUSED(type)
    Q_UNUSED(startColour)
    Q_UNUSED(endColour)
    Q_UNUSED(reverse)
    if (error) *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}

QImage WebGpuContext::stampCloneTile(const QImage &destination,
                                     const QPoint &destinationTileOrigin,
                                     const QImage &sourcePatch,
                                     const QPoint &sourcePatchOrigin,
                                     const QSize &sourceImageSize,
                                     const QVector<QPointF> &stampPoints,
                                     const QTransform &targetPixelToLayer,
                                     const QTransform &targetLayerToDocument,
                                     const QTransform &sourceDocumentToLayer,
                                     const QTransform &sourceLayerToPixel,
                                     const QPointF &sourceOffsetDocument,
                                     const double radius,
                                     const double hardness,
                                     const double opacity,
                                     const CloneStampTarget target,
                                     const CloneStampSample sample,
                                     const int componentIndex,
                                     const bool sourceIsGrey,
                                     const QImage &selectionCoverage,
                                     QString *error)
{
    if (error) error->clear();
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (destination.isNull() || sourcePatch.isNull() || sourceImageSize.isEmpty()
        || stampPoints.isEmpty() || radius <= 0.0) {
        if (error) *error = QStringLiteral("Clone tile input is empty");
        return {};
    }
    if (target == CloneStampTarget::ComponentChannel
        && (componentIndex < 0 || componentIndex > 3)) {
        if (error) *error = QStringLiteral("Clone tile component target is invalid");
        return {};
    }

    QImage destinationRgba;
    if (target == CloneStampTarget::Mask) {
        const QImage gray = destination.convertToFormat(QImage::Format_Grayscale8);
        destinationRgba = QImage(gray.size(), QImage::Format_RGBA8888);
        for (int y = 0; y < gray.height(); ++y) {
            const uchar *sourceRow = gray.constScanLine(y);
            uchar *targetRow = destinationRgba.scanLine(y);
            for (int x = 0; x < gray.width(); ++x) {
                const uchar value = sourceRow[x];
                targetRow[x * 4] = value;
                targetRow[x * 4 + 1] = value;
                targetRow[x * 4 + 2] = value;
                targetRow[x * 4 + 3] = 255;
            }
        }
    } else {
        destinationRgba = destination.convertToFormat(QImage::Format_RGBA8888);
    }
    QImage sourceRgba = sourcePatch.convertToFormat(QImage::Format_RGBA8888);
    QImage selection = selectionCoverage;
    if (selection.isNull() || selection.size() != destinationRgba.size()) {
        selection = QImage(destinationRgba.size(), QImage::Format_RGBA8888);
        selection.fill(QColor(255, 255, 255, 255));
    } else {
        selection = selection.convertToFormat(QImage::Format_RGBA8888);
    }
    if (destinationRgba.isNull() || sourceRgba.isNull() || selection.isNull()) {
        if (error) *error = QStringLiteral("Clone tile images could not be prepared");
        return {};
    }

    const QSize size = destinationRgba.size();
    const uint32_t width = static_cast<uint32_t>(size.width());
    const uint32_t height = static_cast<uint32_t>(size.height());
    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes) * height;

    struct alignas(16) CloneParams {
        float destinationTileOrigin[2];
        float sourcePatchOrigin[2];
        float sourceImageSize[2];
        float sourceOffsetDocument[2];
        float radius;
        float hardness;
        float opacity;
        uint32_t targetMode;
        uint32_t sampleMode;
        int32_t componentIndex;
        uint32_t pointCount;
        uint32_t sourceIsGrey;
        float targetPixelToLayerRows[8];
        float targetLayerToDocumentRows[8];
        float sourceDocumentToLayerRows[8];
        float sourceLayerToPixelRows[8];
    };
    static_assert(sizeof(CloneParams) == 192);

    const auto storeTransform = [](float *rows, const QTransform &transform) {
        rows[0] = static_cast<float>(transform.m11());
        rows[1] = static_cast<float>(transform.m21());
        rows[2] = static_cast<float>(transform.m31());
        rows[3] = 0.0f;
        rows[4] = static_cast<float>(transform.m12());
        rows[5] = static_cast<float>(transform.m22());
        rows[6] = static_cast<float>(transform.m32());
        rows[7] = 0.0f;
    };

    CloneParams params {};
    params.destinationTileOrigin[0] = static_cast<float>(destinationTileOrigin.x());
    params.destinationTileOrigin[1] = static_cast<float>(destinationTileOrigin.y());
    params.sourcePatchOrigin[0] = static_cast<float>(sourcePatchOrigin.x());
    params.sourcePatchOrigin[1] = static_cast<float>(sourcePatchOrigin.y());
    params.sourceImageSize[0] = static_cast<float>(sourceImageSize.width());
    params.sourceImageSize[1] = static_cast<float>(sourceImageSize.height());
    params.sourceOffsetDocument[0] = static_cast<float>(sourceOffsetDocument.x());
    params.sourceOffsetDocument[1] = static_cast<float>(sourceOffsetDocument.y());
    params.radius = static_cast<float>(std::max(0.5, radius));
    params.hardness = static_cast<float>(std::clamp(hardness, 0.0, 0.9999));
    params.opacity = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    params.targetMode = static_cast<uint32_t>(target);
    params.sampleMode = static_cast<uint32_t>(sample);
    params.componentIndex = componentIndex;
    params.pointCount = static_cast<uint32_t>(stampPoints.size());
    params.sourceIsGrey = sourceIsGrey ? 1u : 0u;
    storeTransform(params.targetPixelToLayerRows, targetPixelToLayer);
    storeTransform(params.targetLayerToDocumentRows, targetLayerToDocument);
    storeTransform(params.sourceDocumentToLayerRows, sourceDocumentToLayer);
    storeTransform(params.sourceLayerToPixelRows, sourceLayerToPixel);

    QByteArray pointBytes(static_cast<qsizetype>(stampPoints.size() * 2 * sizeof(float)), '\0');
    auto *pointData = reinterpret_cast<float *>(pointBytes.data());
    for (qsizetype index = 0; index < stampPoints.size(); ++index) {
        pointData[index * 2] = static_cast<float>(stampPoints.at(index).x());
        pointData[index * 2 + 1] = static_cast<float>(stampPoints.at(index).y());
    }

    static constexpr char cloneShaderSource[] = R"WGSL(// Native dirty-tile Clone Stamp kernel. Source and destination are immutable
// straight-RGBA snapshots for the dispatch. Manual bilinear filtering keeps
// transparent neighbouring texels from contaminating visible edge colour.
struct CloneParams {
    destination_tile_origin: vec2<f32>,
    source_patch_origin: vec2<f32>,
    source_image_size: vec2<f32>,
    source_offset_document: vec2<f32>,
    radius: f32,
    hardness: f32,
    opacity: f32,
    target_mode: u32,
    sample_mode: u32,
    component_index: i32,
    point_count: u32,
    source_is_grey: u32,
    target_pixel_to_layer_0: vec4<f32>,
    target_pixel_to_layer_1: vec4<f32>,
    target_layer_to_document_0: vec4<f32>,
    target_layer_to_document_1: vec4<f32>,
    source_document_to_layer_0: vec4<f32>,
    source_document_to_layer_1: vec4<f32>,
    source_layer_to_pixel_0: vec4<f32>,
    source_layer_to_pixel_1: vec4<f32>,
};

struct SourceSample {
    rgba: vec4<f32>,
    valid: u32,
};

@group(0) @binding(0) var destination_tile: texture_2d<f32>;
@group(0) @binding(1) var source_patch: texture_2d<f32>;
@group(0) @binding(2) var output_tile: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(3) var<storage, read> points: array<vec2<f32>>;
@group(0) @binding(4) var<uniform> params: CloneParams;
@group(0) @binding(5) var selection_tile: texture_2d<f32>;

fn affine(point: vec2<f32>, row0: vec4<f32>, row1: vec4<f32>) -> vec2<f32> {
    let homogeneous = vec3<f32>(point, 1.0);
    return vec2<f32>(dot(homogeneous, row0.xyz), dot(homogeneous, row1.xyz));
}

fn quantise_unorm8(value: vec4<f32>) -> vec4<f32> {
    return floor(clamp(value, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0 + 0.5) / 255.0;
}

fn sample_source(full_position: vec2<f32>) -> SourceSample {
    if (full_position.x < 0.0 || full_position.y < 0.0
        || full_position.x > params.source_image_size.x - 1.0
        || full_position.y > params.source_image_size.y - 1.0) {
        return SourceSample(vec4<f32>(0.0), 0u);
    }

    let lower = vec2<i32>(floor(full_position));
    let upper = min(lower + vec2<i32>(1), vec2<i32>(params.source_image_size) - vec2<i32>(1));
    let fraction = clamp(full_position - vec2<f32>(lower), vec2<f32>(0.0), vec2<f32>(1.0));
    let patch_origin = vec2<i32>(params.source_patch_origin);
    let patch_dimensions = vec2<i32>(textureDimensions(source_patch));
    let p00_position = vec2<i32>(lower.x, lower.y) - patch_origin;
    let p10_position = vec2<i32>(upper.x, lower.y) - patch_origin;
    let p01_position = vec2<i32>(lower.x, upper.y) - patch_origin;
    let p11_position = vec2<i32>(upper.x, upper.y) - patch_origin;
    if (any(p00_position < vec2<i32>(0)) || any(p11_position >= patch_dimensions)
        || any(p10_position < vec2<i32>(0)) || any(p10_position >= patch_dimensions)
        || any(p01_position < vec2<i32>(0)) || any(p01_position >= patch_dimensions)) {
        return SourceSample(vec4<f32>(0.0), 0u);
    }

    let p00 = textureLoad(source_patch, p00_position, 0);
    let p10 = textureLoad(source_patch, p10_position, 0);
    let p01 = textureLoad(source_patch, p01_position, 0);
    let p11 = textureLoad(source_patch, p11_position, 0);
    let weights = vec4<f32>((1.0 - fraction.x) * (1.0 - fraction.y),
                            fraction.x * (1.0 - fraction.y),
                            (1.0 - fraction.x) * fraction.y,
                            fraction.x * fraction.y);

    if (params.source_is_grey != 0u) {
        return SourceSample(p00 * weights.x + p10 * weights.y
                            + p01 * weights.z + p11 * weights.w, 1u);
    }

    let alpha = dot(vec4<f32>(p00.a, p10.a, p01.a, p11.a), weights);
    var rgb = p00.rgb * weights.x + p10.rgb * weights.y
        + p01.rgb * weights.z + p11.rgb * weights.w;
    if (alpha > 1.0e-12) {
        rgb = (p00.rgb * p00.a * weights.x + p10.rgb * p10.a * weights.y
               + p01.rgb * p01.a * weights.z + p11.rgb * p11.a * weights.w) / alpha;
    }
    return SourceSample(vec4<f32>(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)),
                                  clamp(alpha, 0.0, 1.0)), 1u);
}

fn scalar_sample(sampled: vec4<f32>) -> f32 {
    if (params.sample_mode == 1u) {
        return clamp(dot(sampled.rgb, vec3<f32>(0.299, 0.587, 0.114)), 0.0, 1.0);
    }
    if (params.sample_mode == 2u) {
        return clamp(sampled.a, 0.0, 1.0);
    }
    if (params.sample_mode == 3u) {
        if (params.component_index == 0) { return clamp(sampled.r, 0.0, 1.0); }
        if (params.component_index == 1) { return clamp(sampled.g, 0.0, 1.0); }
        if (params.component_index == 2) { return clamp(sampled.b, 0.0, 1.0); }
        if (params.component_index == 3) { return clamp(sampled.a, 0.0, 1.0); }
        return 0.0;
    }
    return clamp(sampled.r, 0.0, 1.0);
}

fn blend_straight(destination: vec4<f32>, sampled: vec4<f32>, amount: f32) -> vec4<f32> {
    let inverse_amount = 1.0 - amount;
    let output_alpha = sampled.a * amount + destination.a * inverse_amount;
    var output_rgb = sampled.rgb * amount + destination.rgb * inverse_amount;
    if (output_alpha > 1.0e-12) {
        output_rgb = (sampled.rgb * sampled.a * amount
                      + destination.rgb * destination.a * inverse_amount) / output_alpha;
    }
    return vec4<f32>(clamp(output_rgb, vec3<f32>(0.0), vec3<f32>(1.0)),
                     clamp(output_alpha, 0.0, 1.0));
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_tile);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) {
        return;
    }

    let local = vec2<i32>(gid.xy);
    let target_pixel = params.destination_tile_origin + vec2<f32>(gid.xy) + vec2<f32>(0.5);
    let original = textureLoad(destination_tile, local, 0);
    var edited = original;

    let target_layer = affine(target_pixel,
                              params.target_pixel_to_layer_0,
                              params.target_pixel_to_layer_1);
    let destination_document = affine(target_layer,
                                      params.target_layer_to_document_0,
                                      params.target_layer_to_document_1);
    let source_document = destination_document + params.source_offset_document;
    let source_layer = affine(source_document,
                              params.source_document_to_layer_0,
                              params.source_document_to_layer_1);
    let source_pixel = affine(source_layer,
                              params.source_layer_to_pixel_0,
                              params.source_layer_to_pixel_1) - vec2<f32>(0.5);
    let sampled = sample_source(source_pixel);

    if (sampled.valid != 0u) {
        // A Clone source is fixed for this target pixel throughout the stroke.
        // Combine every overlapping dab into one floating-point coverage value
        // and quantise the completed edit only once. Quantising each
        // very soft dab independently creates channel-specific contour bands.
        var remaining_coverage = 1.0;
        for (var index: u32 = 0u; index < params.point_count; index = index + 1u) {
            let normalised_distance = distance(target_pixel, points[index]) / max(params.radius, 0.5);
            let coverage = 1.0 - smoothstep(min(params.hardness, 0.9999), 1.0, normalised_distance);
            let dab_amount = clamp(coverage * params.opacity, 0.0, 1.0);
            remaining_coverage = remaining_coverage * (1.0 - dab_amount);
        }
        let amount = clamp(1.0 - remaining_coverage, 0.0, 1.0);

        if (amount > 0.0) {
            if (params.target_mode == 0u) {
                edited = blend_straight(original, sampled.rgba, amount);
            } else {
                let value = scalar_sample(sampled.rgba);
                if (params.target_mode == 1u) {
                    edited = vec4<f32>(mix(original.rgb, vec3<f32>(value), amount), original.a);
                } else if (params.target_mode == 2u) {
                    if (params.component_index == 0) { edited.r = mix(original.r, value, amount); }
                    if (params.component_index == 1) { edited.g = mix(original.g, value, amount); }
                    if (params.component_index == 2) { edited.b = mix(original.b, value, amount); }
                    if (params.component_index == 3) { edited.a = mix(original.a, value, amount); }
                } else {
                    let scalar = mix(original.r, value, amount);
                    edited = vec4<f32>(scalar, scalar, scalar, 1.0);
                }
            }
            // Match the authoritative QImage result before selection clipping,
            // but quantise only once for the completed stroke rather than once
            // per overlapping dab.
            edited = quantise_unorm8(edited);
        }
    }

    let selection_amount = clamp(textureLoad(selection_tile, local, 0).r, 0.0, 1.0);
    var output = mix(original, edited, selection_amount);
    if (params.target_mode == 0u) {
        output = blend_straight(original, edited, selection_amount);
    }
    textureStore(output_tile, local, output);
}
)WGSL";

    if (!m_impl->clonePipeline) {
        m_impl->cloneShader = createShaderModule(m_impl->device,
                                                 "PhotoLab tiled Clone Stamp shader",
                                                 cloneShaderSource);
        if (m_impl->cloneShader) {
            WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView("PhotoLab tiled Clone Stamp pipeline");
            descriptor.compute.module = m_impl->cloneShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->clonePipeline = wgpuDeviceCreateComputePipeline(m_impl->device,
                                                                     &descriptor);
        }
        if (!m_impl->clonePipeline) {
            if (error) *error = QStringLiteral("GPU Clone Stamp pipeline creation failed");
            return {};
        }
    }

    WGPUTexture destinationTexture = nullptr;
    WGPUTexture sourceTexture = nullptr;
    WGPUTexture selectionTexture = nullptr;
    WGPUTexture outputTexture = nullptr;
    WGPUTextureView destinationView = nullptr;
    WGPUTextureView sourceView = nullptr;
    WGPUTextureView selectionView = nullptr;
    WGPUTextureView outputView = nullptr;
    WGPUBuffer pointsBuffer = nullptr;
    WGPUBuffer paramsBuffer = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup bindGroup = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;

    auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (bindGroup) wgpuBindGroupRelease(bindGroup);
        if (layout) wgpuBindGroupLayoutRelease(layout);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
        if (pointsBuffer) wgpuBufferRelease(pointsBuffer);
        if (outputView) wgpuTextureViewRelease(outputView);
        if (selectionView) wgpuTextureViewRelease(selectionView);
        if (sourceView) wgpuTextureViewRelease(sourceView);
        if (destinationView) wgpuTextureViewRelease(destinationView);
        if (outputTexture) wgpuTextureRelease(outputTexture);
        if (selectionTexture) wgpuTextureRelease(selectionTexture);
        if (sourceTexture) wgpuTextureRelease(sourceTexture);
        if (destinationTexture) wgpuTextureRelease(destinationTexture);
    };

    destinationTexture = uploadTexture(m_impl->device, m_impl->queue, destinationRgba,
                                       "PhotoLab Clone Stamp destination tile", false, error);
    sourceTexture = uploadTexture(m_impl->device, m_impl->queue, sourceRgba,
                                  "PhotoLab Clone Stamp source patch", false, error);
    selectionTexture = uploadTexture(m_impl->device, m_impl->queue, selection,
                                     "PhotoLab Clone Stamp selection tile", false, error);
    outputTexture = createWorkingTexture(m_impl->device, size,
                                         "PhotoLab Clone Stamp output tile", error);
    destinationView = destinationTexture
        ? wgpuTextureCreateView(destinationTexture, nullptr) : nullptr;
    sourceView = sourceTexture ? wgpuTextureCreateView(sourceTexture, nullptr) : nullptr;
    selectionView = selectionTexture
        ? wgpuTextureCreateView(selectionTexture, nullptr) : nullptr;
    outputView = outputTexture ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
    if (!destinationTexture || !destinationView || !sourceTexture || !sourceView
        || !selectionTexture || !selectionView || !outputTexture || !outputView) {
        cleanup();
        return {};
    }

    WGPUBufferDescriptor pointsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    pointsDescriptor.label = stringView("PhotoLab Clone Stamp points");
    pointsDescriptor.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    pointsDescriptor.size = static_cast<uint64_t>(pointBytes.size());
    pointsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &pointsDescriptor);
    WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    paramsDescriptor.label = stringView("PhotoLab Clone Stamp parameters");
    paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    paramsDescriptor.size = sizeof(params);
    paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab Clone Stamp readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = readbackSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!pointsBuffer || !paramsBuffer || !readbackBuffer) {
        if (error) *error = QStringLiteral("GPU Clone Stamp buffer allocation failed");
        cleanup();
        return {};
    }
    wgpuQueueWriteBuffer(m_impl->queue, pointsBuffer, 0,
                         pointBytes.constData(), static_cast<size_t>(pointBytes.size()));
    wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

    layout = wgpuComputePipelineGetBindGroupLayout(m_impl->clonePipeline, 0);
    WGPUBindGroupEntry entries[6] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = destinationView;
    entries[1].binding = 1; entries[1].textureView = sourceView;
    entries[2].binding = 2; entries[2].textureView = outputView;
    entries[3].binding = 3; entries[3].buffer = pointsBuffer; entries[3].size = pointBytes.size();
    entries[4].binding = 4; entries[4].buffer = paramsBuffer; entries[4].size = sizeof(params);
    entries[5].binding = 5; entries[5].textureView = selectionView;
    WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bindDescriptor.label = stringView("PhotoLab Clone Stamp bind group");
    bindDescriptor.layout = layout;
    bindDescriptor.entryCount = 6;
    bindDescriptor.entries = entries;
    bindGroup = wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor);
    if (!layout || !bindGroup) {
        if (error) *error = QStringLiteral("GPU Clone Stamp bind group creation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("PhotoLab Clone Stamp command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab Clone Stamp compute pass");
    pass = encoder ? wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor) : nullptr;
    if (!encoder || !pass) {
        if (error) *error = QStringLiteral("GPU Clone Stamp command encoding failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, m_impl->clonePipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = outputTexture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource, &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab Clone Stamp tile commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral("GPU Clone Stamp command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage result = mapReadbackBuffer(m_impl->device, readbackBuffer, size,
                                      paddedRowBytes, error);
    cleanup();
    if (!result.isNull()) {
        result.setColorSpace(destination.colorSpace());
    }
    return result;
#else
    Q_UNUSED(destination) Q_UNUSED(destinationTileOrigin) Q_UNUSED(sourcePatch)
    Q_UNUSED(sourcePatchOrigin) Q_UNUSED(sourceImageSize) Q_UNUSED(stampPoints)
    Q_UNUSED(targetPixelToLayer) Q_UNUSED(targetLayerToDocument)
    Q_UNUSED(sourceDocumentToLayer) Q_UNUSED(sourceLayerToPixel)
    Q_UNUSED(sourceOffsetDocument) Q_UNUSED(radius) Q_UNUSED(hardness)
    Q_UNUSED(opacity) Q_UNUSED(target) Q_UNUSED(sample) Q_UNUSED(componentIndex)
    Q_UNUSED(sourceIsGrey) Q_UNUSED(selectionCoverage)
    if (error) *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}

QImage WebGpuContext::featherVectorCoverageTile(
    const VectorFeatherGpuTileData &prepared,
    QString *error)
{
    if (error) error->clear();
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (!prepared.isValid()) {
        if (error) *error = QStringLiteral("The prepared vector Feather tile is invalid");
        return {};
    }

    constexpr int MaximumGpuSupport = 256;
    // Normal canvas tiles are 256 px, while the low-latency interaction path
    // prepares one complete visible viewport. Keep the parity-approved support
    // bounded, but permit a typical high-DPI viewport without forcing every
    // Feather scrub back to CPU solely because it is wider than one cache tile.
    constexpr int MaximumGpuSourceDimension = 3072;
    constexpr int MaximumGpuOutputDimension = 2048;
    const int supportX = static_cast<int>(std::ceil(prepared.radiusX));
    const int supportY = static_cast<int>(std::ceil(prepared.radiusY));
    if (supportX < 1 || supportY < 1
        || supportX > MaximumGpuSupport || supportY > MaximumGpuSupport
        || prepared.coverage.width() > MaximumGpuSourceDimension
        || prepared.coverage.height() > MaximumGpuSourceDimension
        || prepared.colourCarrier.width() > MaximumGpuOutputDimension
        || prepared.colourCarrier.height() > MaximumGpuOutputDimension) {
        if (error) {
            *error = QStringLiteral(
                "Vector Feather exceeds the parity-approved GPU tile guard "
                "(support <= %1 px, source <= %2, output <= %3); exact CPU fallback used")
                         .arg(MaximumGpuSupport)
                         .arg(MaximumGpuSourceDimension)
                         .arg(MaximumGpuOutputDimension);
        }
        return {};
    }

    const auto discreteKernel = [](const int support) {
        QVector<double> values {1.0};
        const int base = support / 3;
        const int remainder = support % 3;
        for (int pass = 0; pass < 3; ++pass) {
            const int radius = base + (pass < remainder ? 1 : 0);
            const int width = radius * 2 + 1;
            QVector<double> next(values.size() + width - 1, 0.0);
            for (qsizetype index = 0; index < values.size(); ++index) {
                const double contribution = values[index] / width;
                for (int tap = 0; tap < width; ++tap) {
                    next[index + tap] += contribution;
                }
            }
            values = std::move(next);
        }
        return values;
    };
    const auto fractionalKernel = [&](const double radius) {
        const int lowSupport = static_cast<int>(std::floor(radius));
        const int highSupport = static_cast<int>(std::ceil(radius));
        const float blend = static_cast<float>(std::clamp(
            radius - std::floor(radius), 0.0, 1.0));
        const QVector<double> low = discreteKernel(lowSupport);
        const QVector<double> high = discreteKernel(highSupport);
        QVector<float> result(highSupport * 2 + 1, 0.0f);
        const int lowOffset = highSupport - lowSupport;
        for (qsizetype index = 0; index < low.size(); ++index) {
            result[lowOffset + index] += static_cast<float>(low[index]) * (1.0f - blend);
        }
        for (qsizetype index = 0; index < high.size(); ++index) {
            result[index] += static_cast<float>(high[index]) * blend;
        }
        return result;
    };

    const QVector<float> kernelX = fractionalKernel(prepared.radiusX);
    const QVector<float> kernelY = fractionalKernel(prepared.radiusY);
    if (kernelX.size() != supportX * 2 + 1
        || kernelY.size() != supportY * 2 + 1) {
        if (error) *error = QStringLiteral("Vector Feather kernel construction failed");
        return {};
    }

    if (!m_impl->vectorFeatherShader) {
        m_impl->vectorFeatherShader = createShaderModule(
            m_impl->device,
            "PhotoLab vector Feather shader",
            gpu_shader::VectorFeather,
            error);
    }
    const auto createPipeline = [&](WGPUComputePipeline *pipeline,
                                    const char *label,
                                    const char *entryPoint) {
        if (*pipeline) return true;
        if (!m_impl->vectorFeatherShader) return false;
        WGPUComputePipelineDescriptor descriptor =
            WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        descriptor.label = stringView(label);
        descriptor.compute.module = m_impl->vectorFeatherShader;
        descriptor.compute.entryPoint = stringView(entryPoint);
        *pipeline = wgpuDeviceCreateComputePipeline(m_impl->device, &descriptor);
        return *pipeline != nullptr;
    };
    if (!createPipeline(&m_impl->vectorFeatherHorizontalPipeline,
                        "PhotoLab vector Feather horizontal pipeline",
                        "horizontal_main")
        || !createPipeline(&m_impl->vectorFeatherVerticalPipeline,
                           "PhotoLab vector Feather vertical pipeline",
                           "vertical_main")) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("GPU vector Feather pipeline creation failed");
        }
        return {};
    }

    struct alignas(16) FeatherParams {
        uint32_t sourceSize[2];
        uint32_t outputSize[2];
        int32_t sourceOrigin[2];
        int32_t outputOrigin[2];
        uint32_t supportX;
        uint32_t supportY;
        uint32_t padding[2];
    };
    static_assert(sizeof(FeatherParams) == 48);
    const QSize sourceSize = prepared.coverage.size();
    const QSize outputSize = prepared.colourCarrier.size();
    FeatherParams params {
        {static_cast<uint32_t>(sourceSize.width()),
         static_cast<uint32_t>(sourceSize.height())},
        {static_cast<uint32_t>(outputSize.width()),
         static_cast<uint32_t>(outputSize.height())},
        {prepared.sourceRect.x(), prepared.sourceRect.y()},
        {prepared.outputRect.x(), prepared.outputRect.y()},
        static_cast<uint32_t>(supportX),
        static_cast<uint32_t>(supportY),
        {0u, 0u}
    };

    const uint32_t width = params.outputSize[0];
    const uint32_t height = params.outputSize[1];
    const uint32_t sourceHeight = params.sourceSize[1];
    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes) * height;
    const uint64_t horizontalCount = static_cast<uint64_t>(width) * sourceHeight;
    if (horizontalCount == 0
        || horizontalCount > std::numeric_limits<uint64_t>::max() / sizeof(float)) {
        if (error) *error = QStringLiteral("GPU vector Feather working buffer is invalid");
        return {};
    }
    const uint64_t horizontalBytes = horizontalCount * sizeof(float);

    WGPUTexture coverageTexture = nullptr;
    WGPUTexture carrierTexture = nullptr;
    WGPUTexture outputTexture = nullptr;
    WGPUTextureView coverageView = nullptr;
    WGPUTextureView carrierView = nullptr;
    WGPUTextureView outputView = nullptr;
    WGPUBuffer kernelXBuffer = nullptr;
    WGPUBuffer kernelYBuffer = nullptr;
    WGPUBuffer horizontalBuffer = nullptr;
    WGPUBuffer paramsBuffer = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUBindGroupLayout horizontalLayout = nullptr;
    WGPUBindGroupLayout verticalLayout = nullptr;
    WGPUBindGroup horizontalBindGroup = nullptr;
    WGPUBindGroup verticalBindGroup = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUComputePassEncoder pass = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;

    const auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (pass) wgpuComputePassEncoderRelease(pass);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        if (verticalBindGroup) wgpuBindGroupRelease(verticalBindGroup);
        if (horizontalBindGroup) wgpuBindGroupRelease(horizontalBindGroup);
        if (verticalLayout) wgpuBindGroupLayoutRelease(verticalLayout);
        if (horizontalLayout) wgpuBindGroupLayoutRelease(horizontalLayout);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (paramsBuffer) wgpuBufferRelease(paramsBuffer);
        if (horizontalBuffer) wgpuBufferRelease(horizontalBuffer);
        if (kernelYBuffer) wgpuBufferRelease(kernelYBuffer);
        if (kernelXBuffer) wgpuBufferRelease(kernelXBuffer);
        if (outputView) wgpuTextureViewRelease(outputView);
        if (carrierView) wgpuTextureViewRelease(carrierView);
        if (coverageView) wgpuTextureViewRelease(coverageView);
        if (outputTexture) wgpuTextureRelease(outputTexture);
        if (carrierTexture) wgpuTextureRelease(carrierTexture);
        if (coverageTexture) wgpuTextureRelease(coverageTexture);
    };

    coverageTexture = uploadTexture(
        m_impl->device, m_impl->queue,
        prepared.coverage.convertToFormat(QImage::Format_RGBA8888),
        "PhotoLab vector Feather coverage", false, error);
    carrierTexture = uploadTexture(
        m_impl->device, m_impl->queue,
        prepared.colourCarrier.convertToFormat(QImage::Format_RGBA8888),
        "PhotoLab vector Feather colour carrier", false, error);
    outputTexture = createWorkingTexture(
        m_impl->device, outputSize,
        "PhotoLab vector Feather output", error);
    coverageView = coverageTexture
        ? wgpuTextureCreateView(coverageTexture, nullptr) : nullptr;
    carrierView = carrierTexture
        ? wgpuTextureCreateView(carrierTexture, nullptr) : nullptr;
    outputView = outputTexture
        ? wgpuTextureCreateView(outputTexture, nullptr) : nullptr;
    if (!coverageTexture || !carrierTexture || !outputTexture
        || !coverageView || !carrierView || !outputView) {
        cleanup();
        return {};
    }

    const auto createStorageBuffer = [&](const char *label,
                                         const uint64_t size,
                                         const auto usage) {
        WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        descriptor.label = stringView(label);
        descriptor.usage = usage;
        descriptor.size = size;
        return wgpuDeviceCreateBuffer(m_impl->device, &descriptor);
    };
    kernelXBuffer = createStorageBuffer(
        "PhotoLab vector Feather X kernel",
        static_cast<uint64_t>(kernelX.size()) * sizeof(float),
        WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    kernelYBuffer = createStorageBuffer(
        "PhotoLab vector Feather Y kernel",
        static_cast<uint64_t>(kernelY.size()) * sizeof(float),
        WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
    horizontalBuffer = createStorageBuffer(
        "PhotoLab vector Feather horizontal values",
        horizontalBytes,
        WGPUBufferUsage_Storage);
    paramsBuffer = createStorageBuffer(
        "PhotoLab vector Feather parameters",
        sizeof(params),
        WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    readbackBuffer = createStorageBuffer(
        "PhotoLab vector Feather readback",
        readbackSize,
        WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
    if (!kernelXBuffer || !kernelYBuffer || !horizontalBuffer
        || !paramsBuffer || !readbackBuffer) {
        if (error) *error = QStringLiteral("GPU vector Feather buffer allocation failed");
        cleanup();
        return {};
    }
    wgpuQueueWriteBuffer(m_impl->queue, kernelXBuffer, 0,
                         kernelX.constData(),
                         static_cast<size_t>(kernelX.size()) * sizeof(float));
    wgpuQueueWriteBuffer(m_impl->queue, kernelYBuffer, 0,
                         kernelY.constData(),
                         static_cast<size_t>(kernelY.size()) * sizeof(float));
    wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

    horizontalLayout = wgpuComputePipelineGetBindGroupLayout(
        m_impl->vectorFeatherHorizontalPipeline, 0);
    verticalLayout = wgpuComputePipelineGetBindGroupLayout(
        m_impl->vectorFeatherVerticalPipeline, 0);
    WGPUBindGroupEntry horizontalEntries[4] = {
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    horizontalEntries[0].binding = 0;
    horizontalEntries[0].textureView = coverageView;
    horizontalEntries[1].binding = 1;
    horizontalEntries[1].buffer = kernelXBuffer;
    horizontalEntries[1].size = static_cast<uint64_t>(kernelX.size()) * sizeof(float);
    horizontalEntries[2].binding = 2;
    horizontalEntries[2].buffer = horizontalBuffer;
    horizontalEntries[2].size = horizontalBytes;
    horizontalEntries[3].binding = 3;
    horizontalEntries[3].buffer = paramsBuffer;
    horizontalEntries[3].size = sizeof(params);
    WGPUBindGroupDescriptor horizontalDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    horizontalDescriptor.label = stringView("PhotoLab vector Feather horizontal bind group");
    horizontalDescriptor.layout = horizontalLayout;
    horizontalDescriptor.entryCount = 4;
    horizontalDescriptor.entries = horizontalEntries;
    horizontalBindGroup = horizontalLayout
        ? wgpuDeviceCreateBindGroup(m_impl->device, &horizontalDescriptor) : nullptr;

    WGPUBindGroupEntry verticalEntries[5] = {
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
        WGPU_BIND_GROUP_ENTRY_INIT};
    verticalEntries[0].binding = 3;
    verticalEntries[0].buffer = paramsBuffer;
    verticalEntries[0].size = sizeof(params);
    verticalEntries[1].binding = 4;
    verticalEntries[1].buffer = horizontalBuffer;
    verticalEntries[1].size = horizontalBytes;
    verticalEntries[2].binding = 5;
    verticalEntries[2].textureView = carrierView;
    verticalEntries[3].binding = 6;
    verticalEntries[3].buffer = kernelYBuffer;
    verticalEntries[3].size = static_cast<uint64_t>(kernelY.size()) * sizeof(float);
    verticalEntries[4].binding = 7;
    verticalEntries[4].textureView = outputView;
    WGPUBindGroupDescriptor verticalDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    verticalDescriptor.label = stringView("PhotoLab vector Feather vertical bind group");
    verticalDescriptor.layout = verticalLayout;
    verticalDescriptor.entryCount = 5;
    verticalDescriptor.entries = verticalEntries;
    verticalBindGroup = verticalLayout
        ? wgpuDeviceCreateBindGroup(m_impl->device, &verticalDescriptor) : nullptr;
    if (!horizontalLayout || !verticalLayout
        || !horizontalBindGroup || !verticalBindGroup) {
        if (error) *error = QStringLiteral("GPU vector Feather bind group creation failed");
        cleanup();
        return {};
    }

    WGPUCommandEncoderDescriptor encoderDescriptor =
        WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("PhotoLab vector Feather command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    if (!encoder) {
        if (error) *error = QStringLiteral("GPU vector Feather command encoder failed");
        cleanup();
        return {};
    }

    WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    passDescriptor.label = stringView("PhotoLab vector Feather horizontal pass");
    pass = wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor);
    if (!pass) {
        if (error) *error = QStringLiteral("GPU vector Feather horizontal pass failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, m_impl->vectorFeatherHorizontalPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, horizontalBindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (width + 7) / 8, (sourceHeight + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    passDescriptor.label = stringView("PhotoLab vector Feather vertical pass");
    pass = wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor);
    if (!pass) {
        if (error) *error = QStringLiteral("GPU vector Feather vertical pass failed");
        cleanup();
        return {};
    }
    wgpuComputePassEncoderSetPipeline(pass, m_impl->vectorFeatherVerticalPipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, verticalBindGroup, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(
        pass, (width + 7) / 8, (height + 7) / 8, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    pass = nullptr;

    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = outputTexture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(
        encoder, &copySource, &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor =
        WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab vector Feather commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral("GPU vector Feather command buffer failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage result = mapReadbackBuffer(
        m_impl->device, readbackBuffer, outputSize, paddedRowBytes, error);
    cleanup();
    if (!result.isNull()) {
        result.setColorSpace(prepared.colourCarrier.colorSpace());
        result.setDevicePixelRatio(prepared.colourCarrier.devicePixelRatio());
        result.setDotsPerMeterX(prepared.colourCarrier.dotsPerMeterX());
        result.setDotsPerMeterY(prepared.colourCarrier.dotsPerMeterY());
    }
    return result;
#else
    Q_UNUSED(prepared)
    if (error) *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}

QImage WebGpuContext::compositeTile(const QImage &base,
                                    const QImage &layer,
                                    const QImage &mask,
                                    const double opacity,
                                    const BlendMode blendMode,
                                    QString *error)
{
    PreparedTileLayer prepared;
    prepared.image = layer;
    prepared.mask = mask;
    prepared.opacity = opacity;
    prepared.blendMode = blendMode;
    PreparedTileLayer background;
    background.image = base;
    background.opacity = 1.0;
    background.blendMode = BlendMode::Copy;
    return compositeHierarchyTile(base.size(), base.colorSpace(), {prepared, background}, error);
}

QImage WebGpuContext::compositeHierarchyTile(const QSize &size,
                                             const QColorSpace &colourSpace,
                                             const QVector<PreparedTileLayer> &layers,
                                             QString *error)
{
    if (error) error->clear();
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) {
        if (error) *error = QStringLiteral("WebGPU device is not ready");
        return {};
    }
    if (size.isEmpty()) {
        if (error) *error = QStringLiteral("Composite tile size is empty");
        return {};
    }

    constexpr int MaximumHierarchyNodes = 1024;
    constexpr int MaximumHierarchyDepth = 64;
    constexpr qsizetype MaximumHierarchyWorkingBytes = qsizetype(256) * 1024 * 1024;
    struct HierarchyCost {
        qsizetype textureCount = 1; // shared transparent input
        int nodeCount = 0;
        int maximumDepth = 0;
    };
    struct PendingPreparedStack {
        const QVector<PreparedTileLayer> *layers = nullptr;
        int depth = 0;
    };
    HierarchyCost hierarchyCost;
    QVector<PendingPreparedStack> pending;
    pending.push_back({&layers, 1});
    while (!pending.isEmpty()) {
        const PendingPreparedStack current = pending.takeLast();
        if (!current.layers) {
            continue;
        }
        hierarchyCost.maximumDepth = std::max(hierarchyCost.maximumDepth, current.depth);
        for (const PreparedTileLayer &item : *current.layers) {
            ++hierarchyCost.nodeCount;
            if (!item.mask.isNull()) {
                ++hierarchyCost.textureCount;
            }
            if (item.isAdjustment()) {
                ++hierarchyCost.textureCount; // adjustment output
            } else if (item.isGroup()) {
                ++hierarchyCost.textureCount; // group composite or before/after mix output
                pending.push_back({&item.children, current.depth + 1});
            } else {
                hierarchyCost.textureCount += 2; // uploaded image plus composite output
            }
        }
    }
    const qsizetype tileBytes = static_cast<qsizetype>(size.width())
        * static_cast<qsizetype>(size.height()) * 4;
    const bool workingSetOverflow = hierarchyCost.textureCount > 0
        && tileBytes > std::numeric_limits<qsizetype>::max() / hierarchyCost.textureCount;
    const qsizetype estimatedWorkingBytes = workingSetOverflow
        ? std::numeric_limits<qsizetype>::max()
        : hierarchyCost.textureCount * tileBytes;
    if (hierarchyCost.nodeCount > MaximumHierarchyNodes
        || hierarchyCost.maximumDepth > MaximumHierarchyDepth
        || estimatedWorkingBytes > MaximumHierarchyWorkingBytes) {
        if (error) {
            *error = QStringLiteral(
                "Native hierarchy resource guard rejected %1 nodes at depth %2 (%3 MiB estimated working textures); CPU fallback used")
                         .arg(hierarchyCost.nodeCount)
                         .arg(hierarchyCost.maximumDepth)
                         .arg(estimatedWorkingBytes / (1024.0 * 1024.0), 0, 'f', 1);
        }
        return {};
    }

    static constexpr char compositeShaderSource[] = R"WGSL(
struct CompositeParams { opacity: f32, blend_mode: u32, use_mask: u32, _padding: u32, };
@group(0) @binding(0) var base_texture: texture_2d<f32>;
@group(0) @binding(1) var layer_texture: texture_2d<f32>;
@group(0) @binding(2) var mask_texture: texture_2d<f32>;
@group(0) @binding(3) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var<uniform> params: CompositeParams;
fn blend_channel(base: f32, effect: f32, mode: u32) -> f32 {
    switch (mode) {
        case 1u: { return base * effect; }
        case 2u: { return 1.0 - (1.0 - base) * (1.0 - effect); }
        case 3u: { return select(2.0 * base * effect, 1.0 - 2.0 * (1.0-base) * (1.0-effect), base > 0.5); }
        case 4u: { return min(base, effect); }
        case 5u: { return max(base, effect); }
        case 6u: { return select(min(1.0, base / max(1.0-effect, 0.00001)), 1.0, effect >= 1.0); }
        case 7u: { return select(1.0-min(1.0,(1.0-base)/max(effect,0.00001)), 0.0, effect <= 0.0); }
        case 8u: { return min(1.0, base + effect); }
        case 9u: { return max(0.0, base - effect); }
        case 10u: { return abs(base - effect); }
        case 11u: { return base + effect - 2.0 * base * effect; }
        default: { return effect; }
    }
}
@compute @workgroup_size(8,8,1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_texture);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) { return; }
    let position = vec2<i32>(gid.xy);
    let base = textureLoad(base_texture, position, 0);
    let layer = textureLoad(layer_texture, position, 0);
    var mask = 1.0;
    if (params.use_mask != 0u) {
        mask = textureLoad(mask_texture, position, 0).r;
    }
    let source_alpha = clamp(layer.a * params.opacity * mask, 0.0, 1.0);
    let backdrop_alpha = clamp(base.a, 0.0, 1.0);
    let blended = vec3<f32>(blend_channel(base.r, layer.r, params.blend_mode),
                            blend_channel(base.g, layer.g, params.blend_mode),
                            blend_channel(base.b, layer.b, params.blend_mode));
    let output_alpha = source_alpha + backdrop_alpha * (1.0 - source_alpha);
    let output_premultiplied = (1.0 - source_alpha) * base.rgb * backdrop_alpha
        + (1.0 - backdrop_alpha) * layer.rgb * source_alpha
        + backdrop_alpha * source_alpha * blended;
    let output_rgb = select(vec3<f32>(0.0),
                            output_premultiplied / max(output_alpha, 0.000001),
                            output_alpha > 0.0);
    textureStore(output_texture, position, vec4<f32>(output_rgb, output_alpha));
}
)WGSL";

    const char *adjustmentShaderSource = gpu_shader::AdjustmentTile;

    static constexpr char passThroughShaderSource[] = R"WGSL(
struct PassThroughParams { opacity: f32, use_mask: u32, _padding0: u32, _padding1: u32, };
@group(0) @binding(0) var before_texture: texture_2d<f32>;
@group(0) @binding(1) var after_texture: texture_2d<f32>;
@group(0) @binding(2) var mask_texture: texture_2d<f32>;
@group(0) @binding(3) var output_texture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(4) var<uniform> params: PassThroughParams;

fn quantize8(value: vec4<f32>) -> vec4<f32> {
    return floor(clamp(value, vec4<f32>(0.0), vec4<f32>(1.0)) * 255.0
                 + vec4<f32>(0.5)) / 255.0;
}

@compute @workgroup_size(8,8,1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dimensions = textureDimensions(output_texture);
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) { return; }
    let position = vec2<i32>(gid.xy);
    let before = textureLoad(before_texture, position, 0);
    let after = textureLoad(after_texture, position, 0);
    var mask = 1.0;
    if (params.use_mask != 0u) {
        mask = textureLoad(mask_texture, position, 0).r;
    }
    let weight = clamp(params.opacity * mask, 0.0, 1.0);

    // ImageProcessor interpolates the premultiplied RGBA8 before/after pixels.
    // Reconstruct and quantize those premultiplied values before applying the
    // same coverage mix, then convert back to the straight-alpha texture form.
    let before_premultiplied = quantize8(vec4<f32>(before.rgb * before.a, before.a));
    let after_premultiplied = quantize8(vec4<f32>(after.rgb * after.a, after.a));
    let mixed = quantize8(before_premultiplied
                          + (after_premultiplied - before_premultiplied) * weight);
    let output_rgb = select(vec3<f32>(0.0),
                            mixed.rgb / max(mixed.a, 0.000001),
                            mixed.a > 0.0);
    textureStore(output_texture, position, vec4<f32>(output_rgb, mixed.a));
}
)WGSL";

    std::function<bool(const QVector<PreparedTileLayer> &)> containsAdjustment;
    containsAdjustment = [&](const QVector<PreparedTileLayer> &stack) {
        for (const PreparedTileLayer &item : stack) {
            if (item.isAdjustment() || (item.isGroup() && containsAdjustment(item.children))) {
                return true;
            }
        }
        return false;
    };
    const bool needsAdjustmentPipeline = containsAdjustment(layers);
    std::function<bool(const QVector<PreparedTileLayer> &)> containsShadowsHighlights;
    containsShadowsHighlights = [&](const QVector<PreparedTileLayer> &stack) {
        for (const PreparedTileLayer &item : stack) {
            if ((item.isAdjustment()
                 && item.adjustmentType == AdjustmentType::ShadowsHighlights)
                || (item.isGroup() && containsShadowsHighlights(item.children))) {
                return true;
            }
        }
        return false;
    };
    const bool needsShadowsPipeline = containsShadowsHighlights(layers);
    std::function<bool(const QVector<PreparedTileLayer> &)> containsPassThrough;
    containsPassThrough = [&](const QVector<PreparedTileLayer> &stack) {
        for (const PreparedTileLayer &item : stack) {
            if (item.isGroup()) {
                if (item.groupCompositeMode == GroupCompositeMode::PassThrough
                    || containsPassThrough(item.children)) {
                    return true;
                }
            }
        }
        return false;
    };
    const bool needsPassThroughPipeline = containsPassThrough(layers);

    if (!m_impl->compositePipeline) {
        m_impl->compositeShader = createShaderModule(m_impl->device,
                                                      "PhotoLab hierarchy compositor shader",
                                                      compositeShaderSource,
                                                      error);
        if (m_impl->compositeShader) {
            WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView("PhotoLab hierarchy compositor pipeline");
            descriptor.compute.module = m_impl->compositeShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->compositePipeline = wgpuDeviceCreateComputePipeline(m_impl->device, &descriptor);
        }
        if (!m_impl->compositePipeline) {
            if (error && error->isEmpty()) *error = QStringLiteral("GPU hierarchy pipeline creation failed");
            return {};
        }
    }
    if (needsAdjustmentPipeline && !m_impl->adjustmentShader) {
        m_impl->adjustmentShader = createShaderModule(m_impl->device,
                                                       "PhotoLab adjustment shader",
                                                       adjustmentShaderSource,
                                                       error);
        if (!m_impl->adjustmentShader) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("GPU adjustment shader creation failed");
            }
            return {};
        }
    }
    const auto createAdjustmentPipeline = [&](WGPUComputePipeline *pipeline,
                                               const char *label,
                                               const char *entryPoint) {
        if (*pipeline) return true;
        WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        descriptor.label = stringView(label);
        descriptor.compute.module = m_impl->adjustmentShader;
        descriptor.compute.entryPoint = stringView(entryPoint);
        *pipeline = wgpuDeviceCreateComputePipeline(m_impl->device, &descriptor);
        return *pipeline != nullptr;
    };
    if (needsAdjustmentPipeline
        && !createAdjustmentPipeline(&m_impl->adjustmentPipeline,
                                     "PhotoLab adjustment pipeline",
                                     "main")) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("GPU adjustment pipeline creation failed");
        }
        return {};
    }
    if (needsShadowsPipeline
        && (!createAdjustmentPipeline(&m_impl->shadowsHorizontalPipeline,
                                      "PhotoLab Shadows/Highlights horizontal pipeline",
                                      "shadows_horizontal")
            || !createAdjustmentPipeline(&m_impl->shadowsApplyPipeline,
                                         "PhotoLab Shadows/Highlights vertical/apply pipeline",
                                         "shadows_vertical_apply"))) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("GPU Shadows/Highlights pipeline creation failed");
        }
        return {};
    }
    if (needsPassThroughPipeline && !m_impl->passThroughPipeline) {
        m_impl->passThroughShader = createShaderModule(m_impl->device,
                                                        "PhotoLab Pass Through shader",
                                                        passThroughShaderSource,
                                                        error);
        if (m_impl->passThroughShader) {
            WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
            descriptor.label = stringView("PhotoLab Pass Through pipeline");
            descriptor.compute.module = m_impl->passThroughShader;
            descriptor.compute.entryPoint = stringView("main");
            m_impl->passThroughPipeline = wgpuDeviceCreateComputePipeline(m_impl->device, &descriptor);
        }
        if (!m_impl->passThroughPipeline) {
            if (error && error->isEmpty()) *error = QStringLiteral("GPU Pass Through pipeline creation failed");
            return {};
        }
    }

    struct TextureHandle { WGPUTexture texture = nullptr; WGPUTextureView view = nullptr; };
    struct alignas(16) CompositeParams {
        float opacity;
        uint32_t blendMode;
        uint32_t useMask;
        uint32_t padding;
    };
    struct alignas(16) PassThroughParams {
        float opacity;
        uint32_t useMask;
        uint32_t padding0;
        uint32_t padding1;
    };
    struct alignas(16) AdjustmentParams {
        float opacity;
        uint32_t blendMode;
        uint32_t useMask;
        uint32_t adjustmentType;

        float exposure;
        float exposureOffset;
        float exposureGamma;
        float contrast;

        float contrastPivot;
        float saturation;
        uint32_t managedDomain;
        uint32_t domainEdgeSize;

        float hueMaster[4];
        float hueRanges[12][4];
        float vibrance[4];
        float photoFilter[4];
        float whiteBalance[4];
        float colourBalance[3][4];
        float colourBalanceOptions[4];
        float channelMixer[4][4];
        float channelMixerOptions[4];
        float blackWhiteWeights0[4];
        float blackWhiteWeights1[4];
        float blackWhiteOptions[4];
        float selectiveColour[9][4];
        float selectiveColourOptions[4];
        float discreteParams[4];
        float shadowsHighlights0[4];
        float shadowsHighlights1[4];
        float lutOptions[4];
        float lutModes[4];
        float lutShaperDomainMin[4];
        float lutShaperDomainMax[4];
        float lutCubeDomainMin[4];
        float lutCubeDomainMax[4];
    };
    static_assert(sizeof(CompositeParams) == 16);
    static_assert(sizeof(PassThroughParams) == 16);
    static_assert(sizeof(AdjustmentParams) == 800);

    const uint32_t width = static_cast<uint32_t>(size.width());
    const uint32_t height = static_cast<uint32_t>(size.height());
    QVector<TextureHandle> textures;
    QVector<WGPUBuffer> buffers;
    QVector<WGPUBindGroup> bindGroups;
    WGPUBindGroupLayout compositeLayout = nullptr;
    WGPUBindGroupLayout adjustmentLayout = nullptr;
    WGPUBindGroupLayout shadowsHorizontalLayout = nullptr;
    WGPUBindGroupLayout shadowsApplyLayout = nullptr;
    WGPUBindGroupLayout passThroughLayout = nullptr;
    WGPUCommandEncoder encoder = nullptr;
    WGPUBuffer readbackBuffer = nullptr;
    WGPUCommandBuffer commandBuffer = nullptr;

    auto registerTexture = [&](WGPUTexture texture) -> TextureHandle {
        TextureHandle handle;
        handle.texture = texture;
        handle.view = texture ? wgpuTextureCreateView(texture, nullptr) : nullptr;
        if (!handle.texture || !handle.view) {
            if (handle.view) wgpuTextureViewRelease(handle.view);
            if (handle.texture) wgpuTextureRelease(handle.texture);
            return {};
        }
        textures.push_back(handle);
        return handle;
    };
    auto cleanup = [&] {
        if (commandBuffer) wgpuCommandBufferRelease(commandBuffer);
        if (readbackBuffer) wgpuBufferRelease(readbackBuffer);
        if (encoder) wgpuCommandEncoderRelease(encoder);
        for (WGPUBindGroup group : bindGroups) wgpuBindGroupRelease(group);
        if (passThroughLayout) wgpuBindGroupLayoutRelease(passThroughLayout);
        if (shadowsApplyLayout) wgpuBindGroupLayoutRelease(shadowsApplyLayout);
        if (shadowsHorizontalLayout) wgpuBindGroupLayoutRelease(shadowsHorizontalLayout);
        if (adjustmentLayout) wgpuBindGroupLayoutRelease(adjustmentLayout);
        if (compositeLayout) wgpuBindGroupLayoutRelease(compositeLayout);
        for (WGPUBuffer buffer : buffers) wgpuBufferRelease(buffer);
        for (const TextureHandle &texture : textures) {
            if (texture.view) wgpuTextureViewRelease(texture.view);
            if (texture.texture) wgpuTextureRelease(texture.texture);
        }
    };

    // Shaders branch before sampling an unused mask, so a 1x1 placeholder is
    // sufficient for unmasked layers and avoids uploading a viewport-sized
    // white image on every interactive adjustment frame.
    QImage whiteMask(QSize(1, 1), QImage::Format_RGBA8888);
    whiteMask.fill(Qt::white);
    TextureHandle whiteMaskHandle = registerTexture(uploadTexture(m_impl->device, m_impl->queue,
                                                                   whiteMask, "PhotoLab white mask",
                                                                   false, error));

    // The common base-image-plus-adjustment case can use the uploaded base
    // image directly as the initial accumulator. Allocate a full-size clear
    // texture only when hierarchy semantics genuinely require one.
    TextureHandle transparentHandle;
    auto ensureTransparent = [&]() -> TextureHandle {
        if (transparentHandle.texture) {
            return transparentHandle;
        }
        QImage transparent(size, QImage::Format_RGBA8888);
        transparent.fill(Qt::transparent);
        transparentHandle = registerTexture(uploadTexture(m_impl->device,
                                                           m_impl->queue,
                                                           transparent,
                                                           "PhotoLab transparent accumulator",
                                                           true,
                                                           error));
        return transparentHandle;
    };

    AdjustmentData identityAdjustment;
    const QImage identityLutImage = buildTonalLookup(identityAdjustment, 8).toRgba8Image();
    TextureHandle identityLutHandle = registerTexture(uploadTexture(m_impl->device, m_impl->queue,
                                                                     identityLutImage,
                                                                     "PhotoLab identity tonal LUT",
                                                                     false, error));
    if (!whiteMaskHandle.texture || !identityLutHandle.texture) {
        cleanup();
        return {};
    }

    compositeLayout = wgpuComputePipelineGetBindGroupLayout(m_impl->compositePipeline, 0);
    if (needsAdjustmentPipeline) {
        adjustmentLayout = wgpuComputePipelineGetBindGroupLayout(m_impl->adjustmentPipeline, 0);
    }
    if (needsShadowsPipeline) {
        shadowsHorizontalLayout = wgpuComputePipelineGetBindGroupLayout(
            m_impl->shadowsHorizontalPipeline, 0);
        shadowsApplyLayout = wgpuComputePipelineGetBindGroupLayout(
            m_impl->shadowsApplyPipeline, 0);
    }
    if (needsPassThroughPipeline) {
        passThroughLayout = wgpuComputePipelineGetBindGroupLayout(m_impl->passThroughPipeline, 0);
    }
    WGPUCommandEncoderDescriptor encoderDescriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoderDescriptor.label = stringView("PhotoLab hierarchy command encoder");
    encoder = wgpuDeviceCreateCommandEncoder(m_impl->device, &encoderDescriptor);
    if (!compositeLayout
        || (needsAdjustmentPipeline && !adjustmentLayout)
        || (needsShadowsPipeline
            && (!shadowsHorizontalLayout || !shadowsApplyLayout))
        || (needsPassThroughPipeline && !passThroughLayout)
        || !encoder) {
        if (error) *error = QStringLiteral("GPU hierarchy command encoder creation failed");
        cleanup();
        return {};
    }

    auto uploadMask = [&](const QImage &input, const bool useMask) -> TextureHandle {
        if (!useMask) {
            return whiteMaskHandle;
        }
        QImage mask = input;
        if (mask.size() != size) {
            mask = mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        return registerTexture(uploadTexture(m_impl->device,
                                             m_impl->queue,
                                             mask.convertToFormat(QImage::Format_RGBA8888),
                                             "PhotoLab hierarchy mask",
                                             false,
                                             error));
    };

    bool failed = false;

    auto createOutputTexture = [&](const char *label) -> TextureHandle {
        TextureHandle output = registerTexture(createWorkingTexture(m_impl->device,
                                                                     size,
                                                                     label,
                                                                     error));
        if (!output.texture) {
            failed = true;
        }
        return output;
    };

    struct ManagedAdjustmentLutViews {
        WGPUTextureView workingToDomain = nullptr;
        WGPUTextureView domainToWorking = nullptr;
        uint32_t edgeSize = 2;

        bool isValid() const
        {
            return workingToDomain && domainToWorking && edgeSize > 1;
        }
    };
    auto resolveManagedAdjustmentLutViews = [&](const PreparedTileLayer &item) {
        ManagedAdjustmentLutViews views;
        if (!item.managedDomainLut) {
            views.workingToDomain = identityLutHandle.view;
            views.domainToWorking = identityLutHandle.view;
            return views;
        }
        const ManagedAdjustmentGpuLutData &lut = *item.managedDomainLut;
        if (!lut.isValid()) {
            if (error) {
                *error = QStringLiteral(
                    "Managed adjustment GPU lattice is incomplete");
            }
            failed = true;
            return ManagedAdjustmentLutViews {};
        }

        auto iterator = m_impl->managedAdjustmentLuts.find(lut.fingerprint);
        if (iterator == m_impl->managedAdjustmentLuts.end()) {
            LutGpuTextureData workingToDomain;
            workingToDomain.size = QSize(lut.edgeSize * lut.edgeSize,
                                         lut.edgeSize);
            workingToDomain.rgba16f = lut.workingToDomainRgba16f;
            LutGpuTextureData domainToWorking;
            domainToWorking.size = workingToDomain.size;
            domainToWorking.rgba16f = lut.domainToWorkingRgba16f;

            Impl::ManagedAdjustmentLutResident resident;
            resident.workingToDomainTexture = uploadLutTexture(
                m_impl->device, m_impl->queue, workingToDomain,
                "PhotoLab working-to-adjustment-domain lattice", error);
            if (resident.workingToDomainTexture) {
                resident.workingToDomainView = wgpuTextureCreateView(
                    resident.workingToDomainTexture, nullptr);
            }
            resident.domainToWorkingTexture = uploadLutTexture(
                m_impl->device, m_impl->queue, domainToWorking,
                "PhotoLab adjustment-domain-to-working lattice", error);
            if (resident.domainToWorkingTexture) {
                resident.domainToWorkingView = wgpuTextureCreateView(
                    resident.domainToWorkingTexture, nullptr);
            }
            if (!resident.workingToDomainTexture
                || !resident.workingToDomainView
                || !resident.domainToWorkingTexture
                || !resident.domainToWorkingView) {
                if (resident.domainToWorkingView) {
                    wgpuTextureViewRelease(resident.domainToWorkingView);
                }
                if (resident.domainToWorkingTexture) {
                    wgpuTextureRelease(resident.domainToWorkingTexture);
                }
                if (resident.workingToDomainView) {
                    wgpuTextureViewRelease(resident.workingToDomainView);
                }
                if (resident.workingToDomainTexture) {
                    wgpuTextureRelease(resident.workingToDomainTexture);
                }
                if (error && error->isEmpty()) {
                    *error = QStringLiteral(
                        "Managed adjustment GPU lattice upload failed");
                }
                failed = true;
                return ManagedAdjustmentLutViews {};
            }
            resident.bytes = (lut.workingToDomainRgba16f.size()
                              + lut.domainToWorkingRgba16f.size())
                * qsizetype(sizeof(qfloat16));
            resident.lastUseSerial = ++m_impl->managedAdjustmentLutUseSerial;
            m_impl->managedAdjustmentLutBytes += resident.bytes;
            m_impl->managedAdjustmentLuts.insert(lut.fingerprint, resident);
            m_impl->evictManagedAdjustmentLuts();
            iterator = m_impl->managedAdjustmentLuts.find(lut.fingerprint);
            if (iterator == m_impl->managedAdjustmentLuts.end()) {
                if (error) {
                    *error = QStringLiteral(
                        "Managed adjustment GPU lattice was evicted before use");
                }
                failed = true;
                return ManagedAdjustmentLutViews {};
            }
        }
        iterator.value().lastUseSerial = ++m_impl->managedAdjustmentLutUseSerial;
        views.workingToDomain = iterator.value().workingToDomainView;
        views.domainToWorking = iterator.value().domainToWorkingView;
        views.edgeSize = static_cast<uint32_t>(lut.edgeSize);
        return views;
    };

    auto encodeAdjustment = [&](const TextureHandle &base,
                                const PreparedTileLayer &item,
                                const TextureHandle &maskHandle,
                                const bool useMask) -> TextureHandle {
        TextureHandle output = createOutputTexture("PhotoLab adjustment output");
        if (!output.texture) return {};

        TextureHandle tonalHandle = identityLutHandle;
        if (item.lutLookup.isValid()) {
            tonalHandle = registerTexture(uploadLutTexture(m_impl->device,
                                                           m_impl->queue,
                                                           item.lutLookup,
                                                           "PhotoLab floating-point LUT",
                                                           error));
            if (!tonalHandle.texture) {
                failed = true;
                return {};
            }
        } else if (!item.tonalLookup.isNull()) {
            tonalHandle = registerTexture(uploadTexture(m_impl->device,
                                                        m_impl->queue,
                                                        item.tonalLookup,
                                                        "PhotoLab adjustment tonal LUT",
                                                        false,
                                                        error));
            if (!tonalHandle.texture) {
                failed = true;
                return {};
            }
        }
        const ManagedAdjustmentLutViews managedLutViews =
            resolveManagedAdjustmentLutViews(item);
        if (!managedLutViews.isValid()) {
            failed = true;
            return {};
        }
        AdjustmentParams params {};
        params.opacity = static_cast<float>(std::clamp(item.opacity, 0.0, 1.0));
        params.blendMode = gpuBlendMode(item.blendMode);
        params.useMask = useMask ? 1u : 0u;
        params.adjustmentType = gpuAdjustmentType(item.adjustmentType);
        params.exposure = static_cast<float>(item.exposureParameters.exposure);
        params.exposureOffset = static_cast<float>(item.exposureParameters.offset);
        params.exposureGamma = static_cast<float>(item.exposureParameters.gamma);
        params.contrast = static_cast<float>(item.contrastParameters.contrast);
        params.contrastPivot = static_cast<float>(item.contrastParameters.pivot);
        params.saturation = static_cast<float>(item.saturationParameters.saturation);
        params.managedDomain = item.managedDomainLut
            ? (item.processingDomain == AdjustmentProcessingDomain::LinearWorking
                   ? 1u : 2u)
            : 0u;
        params.domainEdgeSize = managedLutViews.edgeSize;
        params.hueMaster[0] = static_cast<float>(item.hueSaturationParameters.hue);
        params.hueMaster[1] = static_cast<float>(item.hueSaturationParameters.saturation);
        params.hueMaster[2] = static_cast<float>(item.hueSaturationParameters.lightness);
        for (std::size_t index = 0; index < item.hueSaturationParameters.ranges.size(); ++index) {
            const auto &range = item.hueSaturationParameters.ranges[index];
            params.hueRanges[index * 2][0] = static_cast<float>(range.hue);
            params.hueRanges[index * 2][1] = static_cast<float>(range.saturation);
            params.hueRanges[index * 2][2] = static_cast<float>(range.lightness);
            params.hueRanges[index * 2][3] = static_cast<float>(range.centre);
            params.hueRanges[index * 2 + 1][0] = static_cast<float>(range.width);
            params.hueRanges[index * 2 + 1][1] = static_cast<float>(range.feather);
        }
        params.vibrance[0] = static_cast<float>(item.vibranceParameters.vibrance);
        params.vibrance[1] = static_cast<float>(item.vibranceParameters.saturation);
        params.vibrance[2] = static_cast<float>(item.vibranceParameters.skinProtection);
        params.photoFilter[0] = static_cast<float>(item.photoFilterParameters.colour.redF());
        params.photoFilter[1] = static_cast<float>(item.photoFilterParameters.colour.greenF());
        params.photoFilter[2] = static_cast<float>(item.photoFilterParameters.colour.blueF());
        params.photoFilter[3] = static_cast<float>(item.photoFilterParameters.density / 100.0);
        params.whiteBalance[0] = static_cast<float>(item.whiteBalanceParameters.temperature);
        params.whiteBalance[1] = static_cast<float>(item.whiteBalanceParameters.tint);
        for (std::size_t index = 0; index < item.colourBalanceParameters.ranges.size(); ++index) {
            const auto &range = item.colourBalanceParameters.ranges[index];
            params.colourBalance[index][0] = static_cast<float>(range.cyanRed);
            params.colourBalance[index][1] = static_cast<float>(range.magentaGreen);
            params.colourBalance[index][2] = static_cast<float>(range.yellowBlue);
        }
        params.colourBalanceOptions[0] = item.colourBalanceParameters.preserveLuminosity ? 1.0f : 0.0f;
        for (std::size_t index = 0; index < item.channelMixerParameters.outputs.size(); ++index) {
            const auto &output = item.channelMixerParameters.outputs[index];
            params.channelMixer[index][0] = static_cast<float>(output.red);
            params.channelMixer[index][1] = static_cast<float>(output.green);
            params.channelMixer[index][2] = static_cast<float>(output.blue);
            params.channelMixer[index][3] = static_cast<float>(output.constant);
        }
        params.channelMixer[3][0] = static_cast<float>(item.channelMixerParameters.monochrome.red);
        params.channelMixer[3][1] = static_cast<float>(item.channelMixerParameters.monochrome.green);
        params.channelMixer[3][2] = static_cast<float>(item.channelMixerParameters.monochrome.blue);
        params.channelMixer[3][3] = static_cast<float>(item.channelMixerParameters.monochrome.constant);
        params.channelMixerOptions[0] = item.channelMixerParameters.monochromeEnabled ? 1.0f : 0.0f;
        for (std::size_t index = 0; index < 4; ++index) {
            params.blackWhiteWeights0[index] = static_cast<float>(
                item.blackAndWhiteParameters.colourWeights[index]);
        }
        params.blackWhiteWeights1[0] = static_cast<float>(item.blackAndWhiteParameters.colourWeights[4]);
        params.blackWhiteWeights1[1] = static_cast<float>(item.blackAndWhiteParameters.colourWeights[5]);
        params.blackWhiteWeights1[2] = item.blackAndWhiteParameters.tintEnabled ? 1.0f : 0.0f;
        params.blackWhiteWeights1[3] = static_cast<float>(item.blackAndWhiteParameters.tintHue);
        params.blackWhiteOptions[0] = static_cast<float>(item.blackAndWhiteParameters.tintSaturation);
        for (std::size_t index = 0; index < item.selectiveColourParameters.ranges.size(); ++index) {
            const auto &range = item.selectiveColourParameters.ranges[index];
            params.selectiveColour[index][0] = static_cast<float>(range.cyan);
            params.selectiveColour[index][1] = static_cast<float>(range.magenta);
            params.selectiveColour[index][2] = static_cast<float>(range.yellow);
            params.selectiveColour[index][3] = static_cast<float>(range.black);
        }
        params.selectiveColourOptions[0] =
            item.selectiveColourParameters.method == SelectiveColourMethod::Absolute ? 1.0f : 0.0f;
        params.discreteParams[0] = static_cast<float>(item.posteriseParameters.levels);
        params.discreteParams[1] = static_cast<float>(item.thresholdParameters.threshold);
        params.discreteParams[2] = static_cast<float>(item.thresholdParameters.source);
        params.discreteParams[3] = item.photoFilterParameters.preserveLuminosity ? 1.0f : 0.0f;
        params.shadowsHighlights0[0] = static_cast<float>(item.shadowsHighlightsParameters.shadowAmount);
        params.shadowsHighlights0[1] = static_cast<float>(item.shadowsHighlightsParameters.shadowTonalWidth);
        params.shadowsHighlights0[2] = static_cast<float>(item.shadowsHighlightsParameters.highlightAmount);
        params.shadowsHighlights0[3] = static_cast<float>(item.shadowsHighlightsParameters.highlightTonalWidth);
        params.shadowsHighlights1[0] = static_cast<float>(item.shadowsHighlightsParameters.radius);
        params.shadowsHighlights1[1] = static_cast<float>(item.shadowsHighlightsParameters.midtoneContrast);
        params.shadowsHighlights1[2] = static_cast<float>(item.shadowsHighlightsParameters.colourCorrection);
        params.lutOptions[0] = static_cast<float>(item.lutParameters.strength / 100.0);
        params.lutOptions[1] = static_cast<float>(item.lutParameters.hasShaper()
                                                   ? item.lutParameters.shaperSize : 0);
        params.lutOptions[2] = static_cast<float>(item.lutParameters.hasCube()
                                                   ? item.lutParameters.cubeSize : 0);
        params.lutOptions[3] = static_cast<float>(item.lutLookup.isValid()
                                                   ? item.lutLookup.shaperRow : 0);
        params.lutModes[0] = static_cast<float>(item.lutParameters.interpolation);
        params.lutModes[1] = static_cast<float>(item.lutParameters.processingMode);
        params.lutModes[2] = static_cast<float>(item.lutParameters.operatorProfile);
        params.lutModes[3] = static_cast<float>(item.lutDocumentTransfer);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            params.lutShaperDomainMin[channel] = static_cast<float>(
                item.lutParameters.shaperDomainMin[channel]);
            params.lutShaperDomainMax[channel] = static_cast<float>(
                item.lutParameters.shaperDomainMax[channel]);
            params.lutCubeDomainMin[channel] = static_cast<float>(
                item.lutParameters.cubeDomainMin[channel]);
            params.lutCubeDomainMax[channel] = static_cast<float>(
                item.lutParameters.cubeDomainMax[channel]);
        }
        WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        paramsDescriptor.label = stringView("PhotoLab adjustment parameters");
        paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        paramsDescriptor.size = sizeof(params);
        WGPUBuffer paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
        if (!paramsBuffer) { failed = true; return {}; }
        buffers.push_back(paramsBuffer);
        wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

        const auto encodeAdjustmentPass = [&](WGPUComputePipeline pipeline,
                                              WGPUBindGroupLayout layout,
                                              WGPUTextureView lookupView,
                                              WGPUTextureView outputView,
                                              const char *bindLabel,
                                              const char *passLabel) {
            WGPUBindGroupEntry entries[7] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT};
            entries[0].binding = 0; entries[0].textureView = base.view;
            entries[1].binding = 1; entries[1].textureView = maskHandle.view;
            entries[2].binding = 2; entries[2].textureView = lookupView;
            entries[3].binding = 3; entries[3].textureView = outputView;
            entries[4].binding = 4; entries[4].buffer = paramsBuffer;
            entries[4].size = sizeof(params);
            entries[5].binding = 5;
            entries[5].textureView = managedLutViews.workingToDomain;
            entries[6].binding = 6;
            entries[6].textureView = managedLutViews.domainToWorking;
            WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bindDescriptor.label = stringView(bindLabel);
            bindDescriptor.layout = layout;
            bindDescriptor.entryCount = 7;
            bindDescriptor.entries = entries;
            WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(m_impl->device,
                                                                 &bindDescriptor);
            if (!bindGroup) return false;
            bindGroups.push_back(bindGroup);

            WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
            passDescriptor.label = stringView(passLabel);
            WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(
                encoder, &passDescriptor);
            if (!pass) return false;
            wgpuComputePassEncoderSetPipeline(pass, pipeline);
            wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass, (width + 7) / 8, (height + 7) / 8, 1);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            return true;
        };

        const auto encodeShadowsHorizontalPass = [&](WGPUTextureView outputView) {
            WGPUBindGroupEntry entries[4] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT};
            entries[0].binding = 0; entries[0].textureView = base.view;
            entries[1].binding = 3; entries[1].textureView = outputView;
            entries[2].binding = 4; entries[2].buffer = paramsBuffer;
            entries[2].size = sizeof(params);
            entries[3].binding = 5;
            entries[3].textureView = managedLutViews.workingToDomain;
            WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            bindDescriptor.label = stringView(
                "PhotoLab Shadows/Highlights horizontal bind group");
            bindDescriptor.layout = shadowsHorizontalLayout;
            bindDescriptor.entryCount = 4;
            bindDescriptor.entries = entries;
            WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(m_impl->device,
                                                                 &bindDescriptor);
            if (!bindGroup) return false;
            bindGroups.push_back(bindGroup);
            WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
            passDescriptor.label = stringView(
                "PhotoLab Shadows/Highlights horizontal pass");
            WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(
                encoder, &passDescriptor);
            if (!pass) return false;
            wgpuComputePassEncoderSetPipeline(pass, m_impl->shadowsHorizontalPipeline);
            wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass, (width + 7) / 8, (height + 7) / 8, 1);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
            return true;
        };

        if (item.adjustmentType == AdjustmentType::ShadowsHighlights
            && (std::abs(item.shadowsHighlightsParameters.shadowAmount) > 1.0e-12
                || std::abs(item.shadowsHighlightsParameters.highlightAmount) > 1.0e-12
                || std::abs(item.shadowsHighlightsParameters.midtoneContrast) > 1.0e-12)) {
            TextureHandle horizontal = createOutputTexture(
                "PhotoLab Shadows/Highlights horizontal adaptation");
            if (!horizontal.texture
                || !encodeShadowsHorizontalPass(horizontal.view)
                || !encodeAdjustmentPass(
                    m_impl->shadowsApplyPipeline,
                    shadowsApplyLayout,
                    horizontal.view,
                    output.view,
                    "PhotoLab Shadows/Highlights vertical/apply bind group",
                    "PhotoLab Shadows/Highlights vertical/apply pass")) {
                failed = true;
                return {};
            }
            return output;
        }

        if (!encodeAdjustmentPass(m_impl->adjustmentPipeline,
                                  adjustmentLayout,
                                  tonalHandle.view,
                                  output.view,
                                  "PhotoLab adjustment bind group",
                                  "PhotoLab adjustment pass")) {
            failed = true;
            return {};
        }
        return output;
    };

    auto encodeComposite = [&](const TextureHandle &base,
                               const TextureHandle &layer,
                               const PreparedTileLayer &item,
                               const TextureHandle &maskHandle,
                               const bool useMask) -> TextureHandle {
        TextureHandle output = createOutputTexture("PhotoLab hierarchy composite output");
        if (!output.texture) return {};

        CompositeParams params {static_cast<float>(std::clamp(item.opacity, 0.0, 1.0)),
                                gpuBlendMode(item.blendMode),
                                useMask ? 1u : 0u,
                                0u};
        WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        paramsDescriptor.label = stringView("PhotoLab hierarchy parameters");
        paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        paramsDescriptor.size = sizeof(params);
        WGPUBuffer paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
        if (!paramsBuffer) { failed = true; return {}; }
        buffers.push_back(paramsBuffer);
        wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

        WGPUBindGroupEntry entries[5] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0; entries[0].textureView = base.view;
        entries[1].binding = 1; entries[1].textureView = layer.view;
        entries[2].binding = 2; entries[2].textureView = maskHandle.view;
        entries[3].binding = 3; entries[3].textureView = output.view;
        entries[4].binding = 4; entries[4].buffer = paramsBuffer;
        entries[4].size = sizeof(params);
        WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bindDescriptor.label = stringView("PhotoLab hierarchy bind group");
        bindDescriptor.layout = compositeLayout;
        bindDescriptor.entryCount = 5;
        bindDescriptor.entries = entries;
        WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor);
        if (!bindGroup) { failed = true; return {}; }
        bindGroups.push_back(bindGroup);

        WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        passDescriptor.label = stringView("PhotoLab hierarchy pass");
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor);
        if (!pass) { failed = true; return {}; }
        wgpuComputePassEncoderSetPipeline(pass, m_impl->compositePipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        return output;
    };

    auto encodePassThrough = [&](const TextureHandle &before,
                                 const TextureHandle &after,
                                 const PreparedTileLayer &item,
                                 const TextureHandle &maskHandle,
                                 const bool useMask) -> TextureHandle {
        TextureHandle output = createOutputTexture("PhotoLab Pass Through output");
        if (!output.texture) return {};

        PassThroughParams params {static_cast<float>(std::clamp(item.opacity, 0.0, 1.0)),
                                  useMask ? 1u : 0u,
                                  0u,
                                  0u};
        WGPUBufferDescriptor paramsDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        paramsDescriptor.label = stringView("PhotoLab Pass Through parameters");
        paramsDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        paramsDescriptor.size = sizeof(params);
        WGPUBuffer paramsBuffer = wgpuDeviceCreateBuffer(m_impl->device, &paramsDescriptor);
        if (!paramsBuffer) { failed = true; return {}; }
        buffers.push_back(paramsBuffer);
        wgpuQueueWriteBuffer(m_impl->queue, paramsBuffer, 0, &params, sizeof(params));

        WGPUBindGroupEntry entries[5] = {WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT,
                                         WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0; entries[0].textureView = before.view;
        entries[1].binding = 1; entries[1].textureView = after.view;
        entries[2].binding = 2; entries[2].textureView = maskHandle.view;
        entries[3].binding = 3; entries[3].textureView = output.view;
        entries[4].binding = 4; entries[4].buffer = paramsBuffer;
        entries[4].size = sizeof(params);
        WGPUBindGroupDescriptor bindDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bindDescriptor.label = stringView("PhotoLab Pass Through bind group");
        bindDescriptor.layout = passThroughLayout;
        bindDescriptor.entryCount = 5;
        bindDescriptor.entries = entries;
        WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(m_impl->device, &bindDescriptor);
        if (!bindGroup) { failed = true; return {}; }
        bindGroups.push_back(bindGroup);

        WGPUComputePassDescriptor passDescriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        passDescriptor.label = stringView("PhotoLab Pass Through pass");
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, &passDescriptor);
        if (!pass) { failed = true; return {}; }
        wgpuComputePassEncoderSetPipeline(pass, m_impl->passThroughPipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, (width + 7) / 8, (height + 7) / 8, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        return output;
    };

    // Accumulators are immutable once emitted. Pass Through therefore keeps
    // the exact parent texture as its before input while recursive child work
    // produces independent textures. Isolated recursion starts empty and only
    // requests a transparent accumulator when its contents require one.
    std::function<TextureHandle(const QVector<PreparedTileLayer> &, const TextureHandle &)> encodeStack;
    encodeStack = [&](const QVector<PreparedTileLayer> &stack,
                      const TextureHandle &initial) -> TextureHandle {
        TextureHandle current = initial;
        for (auto iterator = stack.crbegin(); iterator != stack.crend(); ++iterator) {
            const PreparedTileLayer &item = *iterator;
            const bool useMask = !item.mask.isNull();
            const TextureHandle maskHandle = uploadMask(item.mask, useMask);
            if (!maskHandle.texture) { failed = true; return {}; }

            if (item.isAdjustment()) {
                if (!current.texture) {
                    current = ensureTransparent();
                    if (!current.texture) { failed = true; return {}; }
                }
                current = encodeAdjustment(current, item, maskHandle, useMask);
                if (!current.texture) return {};
                continue;
            }

            if (item.isGroup()
                && item.groupCompositeMode == GroupCompositeMode::PassThrough) {
                if (!current.texture) {
                    current = ensureTransparent();
                    if (!current.texture) { failed = true; return {}; }
                }
                const TextureHandle before = current;
                const TextureHandle after = encodeStack(item.children, before);
                if (!after.texture) { failed = true; return {}; }
                current = encodePassThrough(before, after, item, maskHandle, useMask);
                if (!current.texture) return {};
                continue;
            }

            TextureHandle layerHandle;
            if (item.isGroup()) {
                layerHandle = encodeStack(item.children, {});
            } else {
                if (item.residentTileKey != 0) {
                    auto resident = m_impl->residentTiles.find(item.residentTileKey);
                    if (resident != m_impl->residentTiles.end()
                        && resident.value().revision == item.residentTileRevision
                        && resident.value().size == size
                        && resident.value().texture
                        && resident.value().view) {
                        resident.value().lastUseSerial = ++m_impl->residentUseSerial;
                        // Borrow the bounded resident Smart tile. It is owned by
                        // Impl::residentTiles and must not enter the temporary
                        // texture cleanup vector for this command submission.
                        layerHandle.texture = resident.value().texture;
                        layerHandle.view = resident.value().view;
                    }
                }
                if (!layerHandle.texture) {
                    QImage layerImage = item.image;
                    if (layerImage.size() != size) {
                        layerImage = layerImage.scaled(size,
                                                       Qt::IgnoreAspectRatio,
                                                       Qt::SmoothTransformation);
                    }
                    layerHandle = registerTexture(uploadTexture(m_impl->device,
                                                                 m_impl->queue,
                                                                 layerImage.convertToFormat(
                                                                     QImage::Format_RGBA8888),
                                                                 "PhotoLab hierarchy layer",
                                                                 false,
                                                                 error));
                }
            }
            if (!layerHandle.texture) { failed = true; return {}; }

            // A fully opaque, unmasked Copy layer at the bottom is already the
            // exact accumulator texture. Reuse it directly instead of creating
            // and compositing through a full-size transparent texture.
            if (!current.texture
                && !useMask
                && item.blendMode == BlendMode::Copy
                && item.opacity >= 1.0) {
                current = layerHandle;
                continue;
            }
            if (!current.texture) {
                current = ensureTransparent();
                if (!current.texture) { failed = true; return {}; }
            }
            current = encodeComposite(current, layerHandle, item, maskHandle, useMask);
            if (!current.texture) return {};
        }
        if (!current.texture) {
            current = ensureTransparent();
            if (!current.texture) failed = true;
        }
        return current;
    };

    TextureHandle finalTexture = encodeStack(layers, {});
    if (failed || !finalTexture.texture) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("GPU hierarchy encoding failed");
        }
        cleanup();
        return {};
    }

    const uint32_t paddedRowBytes = alignedBytesPerRow(width);
    const uint64_t readbackSize = static_cast<uint64_t>(paddedRowBytes) * height;
    WGPUBufferDescriptor readbackDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    readbackDescriptor.label = stringView("PhotoLab hierarchy readback");
    readbackDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    readbackDescriptor.size = readbackSize;
    readbackBuffer = wgpuDeviceCreateBuffer(m_impl->device, &readbackDescriptor);
    if (!readbackBuffer) {
        if (error) *error = QStringLiteral("GPU hierarchy readback allocation failed");
        cleanup();
        return {};
    }
    WGPUTexelCopyTextureInfo copySource = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    copySource.texture = finalTexture.texture;
    WGPUTexelCopyBufferInfo copyDestination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    copyDestination.buffer = readbackBuffer;
    copyDestination.layout.bytesPerRow = paddedRowBytes;
    copyDestination.layout.rowsPerImage = height;
    const WGPUExtent3D extent {width, height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &copySource, &copyDestination, &extent);
    WGPUCommandBufferDescriptor commandDescriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    commandDescriptor.label = stringView("PhotoLab hierarchy commands");
    commandBuffer = wgpuCommandEncoderFinish(encoder, &commandDescriptor);
    wgpuCommandEncoderRelease(encoder);
    encoder = nullptr;
    if (!commandBuffer) {
        if (error) *error = QStringLiteral("GPU hierarchy command buffer creation failed");
        cleanup();
        return {};
    }
    wgpuQueueSubmit(m_impl->queue, 1, &commandBuffer);
    QImage result = mapReadbackBuffer(m_impl->device, readbackBuffer, size,
                                      paddedRowBytes, error);
    cleanup();
    if (!result.isNull()) result.setColorSpace(colourSpace);
    return result;
#else
    Q_UNUSED(size) Q_UNUSED(colourSpace) Q_UNUSED(layers)
    if (error) *error = QStringLiteral("VFX Photo Lab was built without wgpu-native");
    return {};
#endif
}

void WebGpuContext::cacheResidentTile(const quint64 residencyKey,
                                      const quint64 revision,
                                      const QImage &image)
{
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    if (residencyKey == 0 || image.isNull()) return;
    std::lock_guard operationLock(m_impl->operationMutex);
    if (!deviceReady()) return;
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    auto existing = m_impl->residentTiles.find(residencyKey);
    if (existing != m_impl->residentTiles.end()
        && existing.value().revision == revision
        && existing.value().size == rgba.size()
        && existing.value().texture
        && existing.value().view) {
        existing.value().lastUseSerial = ++m_impl->residentUseSerial;
        return;
    }
    QString ignored;
    WGPUTexture texture = uploadTexture(m_impl->device, m_impl->queue, rgba,
                                        "PhotoLab resident tile", false, &ignored);
    WGPUTextureView view = texture ? wgpuTextureCreateView(texture, nullptr) : nullptr;
    if (!texture || !view) {
        if (view) wgpuTextureViewRelease(view);
        if (texture) wgpuTextureRelease(texture);
        return;
    }
    m_impl->removeResidentTile(residencyKey);
    Impl::ResidentTile tile;
    tile.texture = texture;
    tile.view = view;
    tile.revision = revision;
    tile.size = rgba.size();
    tile.bytes = rgba.sizeInBytes();
    tile.lastUseSerial = ++m_impl->residentUseSerial;
    if (tile.bytes <= 0 || tile.bytes > m_impl->residentBudget) {
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(texture);
        return;
    }
    m_impl->residentTiles.insert(residencyKey, tile);
    m_impl->residentBytes += tile.bytes;
    m_impl->evictResidentTiles();
#else
    Q_UNUSED(residencyKey) Q_UNUSED(revision) Q_UNUSED(image)
#endif
}

void WebGpuContext::invalidateResidentTile(const quint64 residencyKey)
{
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    m_impl->removeResidentTile(residencyKey);
#else
    Q_UNUSED(residencyKey)
#endif
}

qsizetype WebGpuContext::residentVramBytes() const
{
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    return m_impl->residentBytes;
#else
    return 0;
#endif
}

int WebGpuContext::residentTileCount() const
{
#ifdef VFXPHOTOLAB_HAS_WEBGPU
    std::lock_guard operationLock(m_impl->operationMutex);
    return m_impl->residentTiles.size();
#else
    return 0;
#endif
}


bool WebGpuContext::runTileParitySelfTest(QString *details)
{
    if (details) {
        details->clear();
    }
    m_impl->approvedAdjustmentMask = 0;
    m_impl->fillSelfTestPassed = false;
    m_impl->gradientSelfTestPassed = false;
    m_impl->vectorFeatherSelfTestPassed = false;
    m_impl->displayTransformSelfTestPassed = false;
    m_impl->managedAdjustmentTransformSelfTestPassed = false;
    if (!deviceReady()) {
        if (details) *details = m_impl->status;
        return false;
    }

    qInfo().noquote() << "[GPU diagnostic] Beginning 64x64 CPU/GPU tile parity test.";
    QImage cpuTile(64, 64, QImage::Format_RGBA8888);
    for (int y = 0; y < cpuTile.height(); ++y) {
        for (int x = 0; x < cpuTile.width(); ++x) {
            cpuTile.setPixelColor(x,
                                  y,
                                  QColor((x * 17 + y * 3) & 255,
                                         (x * 5 + y * 29) & 255,
                                         (x * 11 + y * 7) & 255,
                                         (x * 13 + y * 19) & 255));
        }
    }

    QString error;
    const QImage gpuTile = roundTripTile(cpuTile, &error);
    if (gpuTile.isNull()) {
        m_impl->selfTestPassed = false;
        m_impl->status = QStringLiteral("Native WebGPU device ready, but isolated tile validation failed: %1; CPU renderer active")
                             .arg(error);
        if (details) *details = m_impl->status;
        return false;
    }

    struct ParityDifference {
        int maximum = 255;
        qsizetype differingChannels = 0;
        int x = -1;
        int y = -1;
        int channel = -1;
        int cpuValue = 0;
        int gpuValue = 0;
    };
    const auto compareChannels = [](const QImage &cpu, const QImage &gpu) {
        ParityDifference report;
        if (cpu.isNull() || gpu.isNull() || cpu.size() != gpu.size()) {
            return report;
        }
        report.maximum = 0;
        for (int y = 0; y < cpu.height(); ++y) {
            for (int x = 0; x < cpu.width(); ++x) {
                const QColor expected = cpu.pixelColor(x, y);
                const QColor actual = gpu.pixelColor(x, y);
                const int cpuValues[] {expected.red(), expected.green(),
                                       expected.blue(), expected.alpha()};
                const int gpuValues[] {actual.red(), actual.green(),
                                       actual.blue(), actual.alpha()};
                for (int channel = 0; channel < 4; ++channel) {
                    const int difference = std::abs(cpuValues[channel]
                                                    - gpuValues[channel]);
                    if (difference > 0) {
                        ++report.differingChannels;
                    }
                    if (difference > report.maximum) {
                        report.maximum = difference;
                        report.x = x;
                        report.y = y;
                        report.channel = channel;
                        report.cpuValue = cpuValues[channel];
                        report.gpuValue = gpuValues[channel];
                    }
                }
            }
        }
        return report;
    };

    const ParityDifference identityReport = compareChannels(cpuTile, gpuTile);
    const int identityDifference = identityReport.maximum;
    if (identityDifference > 1) {
        m_impl->selfTestPassed = false;
        m_impl->status = QStringLiteral(
            "Native WebGPU device ready, but tile parity differed by %1 channel values; CPU renderer active")
                             .arg(identityDifference);
        if (details) *details = m_impl->status;
        qInfo().noquote() << "[GPU diagnostic]" << m_impl->status;
        return false;
    }

    qInfo().noquote() << "[GPU diagnostic] Beginning tiled resize CPU/GPU parity test.";
    QImage resizeSource(513, 7, QImage::Format_RGBA8888);
    for (int y = 0; y < resizeSource.height(); ++y) {
        uchar *row = resizeSource.scanLine(y);
        for (int x = 0; x < resizeSource.width(); ++x) {
            row[x * 4] = static_cast<uchar>((17 + x * 11 + y * 3) & 255);
            row[x * 4 + 1] = static_cast<uchar>((31 + x * 5 + y * 19) & 255);
            row[x * 4 + 2] = static_cast<uchar>((73 + x * 7 + y * 13) & 255);
            row[x * 4 + 3] = static_cast<uchar>((x + y) % 5 == 0
                                                    ? 0
                                                    : ((x * 3 + y * 29) & 255));
        }
    }
    const QSize resizeDestination(521, 11);
    int resizeDifference = 0;
    const ImageResampleMethod resizeMethods[] {
        ImageResampleMethod::NearestNeighbour,
        ImageResampleMethod::Bilinear
    };
    for (const ImageResampleMethod resizeMethod : resizeMethods) {
        const QImage cpuResized = resampleStraightRgbaCpuReference(
            resizeSource, resizeDestination, resizeMethod);
        error.clear();
        const QImage gpuResized = resampleImageTiled(
            resizeSource, resizeDestination, resizeMethod, nullptr, &error);
        if (gpuResized.isNull()) {
            m_impl->selfTestPassed = false;
            m_impl->status = QStringLiteral(
                "Native WebGPU tile parity passed, but tiled %1 resize validation failed: %2; CPU renderer active")
                                 .arg(resizeMethod == ImageResampleMethod::NearestNeighbour
                                          ? QStringLiteral("Nearest")
                                          : QStringLiteral("Bilinear"),
                                      error);
            if (details) *details = m_impl->status;
            qInfo().noquote() << "[GPU diagnostic]" << m_impl->status;
            return false;
        }
        const ParityDifference resizeReport = compareChannels(cpuResized, gpuResized);
        resizeDifference = std::max(resizeDifference, resizeReport.maximum);
        if (resizeReport.maximum > 1) {
            m_impl->selfTestPassed = false;
            m_impl->status = QStringLiteral(
                "Native WebGPU tiled %1 resize parity differed by %2 channel values; CPU renderer active")
                                 .arg(resizeMethod == ImageResampleMethod::NearestNeighbour
                                          ? QStringLiteral("Nearest")
                                          : QStringLiteral("Bilinear"))
                                 .arg(resizeReport.maximum);
            if (details) *details = m_impl->status;
            qInfo().noquote() << "[GPU diagnostic]" << m_impl->status;
            return false;
        }
    }

    qInfo().noquote() << "[GPU diagnostic] Beginning WGSL display colour-transform parity test.";
    int displayDifference8 = -1;
    int displayDifferencePremultiplied = -1;
    int displayDifference16 = -1;
    QString displayValidationError;
    DocumentColourState displayState = DocumentColourState::managedForImage(
        QColorSpace(QColorSpace::SRgb));
    displayState.presentationColourManagementEnabled = true;
    displayState.displayTransform.kind = DisplayTransformKind::IccProfile;
    displayState.displayTransform.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::DisplayP3));
    MonitorProfileInfo displayMonitor;
    displayMonitor.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    const auto displayTransform = createDisplayColourTransform(
        displayState, displayMonitor, &displayValidationError);
    if (displayTransform) {
        const auto displayLut = displayTransform->gpuLutData(
            &displayValidationError);
        if (displayLut) {
            QImage displaySource8(71, 53, QImage::Format_RGBA8888);
            for (int y = 0; y < displaySource8.height(); ++y) {
                uchar *row = displaySource8.scanLine(y);
                for (int x = 0; x < displaySource8.width(); ++x) {
                    row[x * 4] = static_cast<uchar>((x * 17 + y * 5 + 11) & 255);
                    row[x * 4 + 1] = static_cast<uchar>((x * 3 + y * 23 + 47) & 255);
                    row[x * 4 + 2] = static_cast<uchar>((x * 29 + y * 7 + 83) & 255);
                    row[x * 4 + 3] = static_cast<uchar>((x * 13 + y * 19) & 255);
                }
            }
            displaySource8.setColorSpace(QColorSpace(QColorSpace::SRgb));
            QImage displayCpu8 = displaySource8;
            QString cpuError;
            const bool cpuOk8 = displayTransform->apply(
                &displayCpu8, nullptr, &cpuError);
            QString gpuError;
            const QImage displayGpu8 = applyDisplayColourTransform(
                displaySource8, *displayLut, nullptr, &gpuError);
            if (cpuOk8 && !displayGpu8.isNull()) {
                displayDifference8 = compareChannels(
                    displayCpu8, displayGpu8).maximum;
            } else if (displayValidationError.isEmpty()) {
                displayValidationError = !gpuError.isEmpty() ? gpuError : cpuError;
            }

            const QImage displaySourcePremultiplied = displaySource8.convertToFormat(
                QImage::Format_ARGB32_Premultiplied);
            QImage displayCpuPremultiplied = displaySourcePremultiplied;
            cpuError.clear();
            const bool cpuOkPremultiplied = displayTransform->apply(
                &displayCpuPremultiplied, nullptr, &cpuError);
            gpuError.clear();
            const QImage displayGpuPremultiplied = applyDisplayColourTransform(
                displaySourcePremultiplied, *displayLut, nullptr, &gpuError);
            if (cpuOkPremultiplied && !displayGpuPremultiplied.isNull()) {
                displayDifferencePremultiplied = compareChannels(
                    displayCpuPremultiplied,
                    displayGpuPremultiplied).maximum;
            } else if (displayValidationError.isEmpty()) {
                displayValidationError = !gpuError.isEmpty() ? gpuError : cpuError;
            }

            QImage displaySource16(43, 31, QImage::Format_RGBA64);
            for (int y = 0; y < displaySource16.height(); ++y) {
                auto *row = reinterpret_cast<QRgba64 *>(displaySource16.scanLine(y));
                for (int x = 0; x < displaySource16.width(); ++x) {
                    row[x] = QRgba64::fromRgba64(
                        static_cast<quint16>((x * 1777 + y * 313 + 101) & 65535),
                        static_cast<quint16>((x * 547 + y * 2029 + 1301) & 65535),
                        static_cast<quint16>((x * 2591 + y * 881 + 4093) & 65535),
                        static_cast<quint16>((x * 937 + y * 1237 + 17) & 65535));
                }
            }
            displaySource16.setColorSpace(QColorSpace(QColorSpace::SRgb));
            QImage displayCpu16 = displaySource16;
            cpuError.clear();
            const bool cpuOk16 = displayTransform->apply(
                &displayCpu16, nullptr, &cpuError);
            gpuError.clear();
            const QImage displayGpu16 = applyDisplayColourTransform(
                displaySource16, *displayLut, nullptr, &gpuError);
            if (cpuOk16 && !displayGpu16.isNull()) {
                const QImage cpu64 = displayCpu16.convertToFormat(QImage::Format_RGBA64);
                const QImage gpu64 = displayGpu16.convertToFormat(QImage::Format_RGBA64);
                displayDifference16 = 0;
                for (int y = 0; y < cpu64.height(); ++y) {
                    const auto *cpuRow = reinterpret_cast<const QRgba64 *>(
                        cpu64.constScanLine(y));
                    const auto *gpuRow = reinterpret_cast<const QRgba64 *>(
                        gpu64.constScanLine(y));
                    for (int x = 0; x < cpu64.width(); ++x) {
                        displayDifference16 = std::max({
                            displayDifference16,
                            std::abs(int(cpuRow[x].red()) - int(gpuRow[x].red())),
                            std::abs(int(cpuRow[x].green()) - int(gpuRow[x].green())),
                            std::abs(int(cpuRow[x].blue()) - int(gpuRow[x].blue())),
                            std::abs(int(cpuRow[x].alpha()) - int(gpuRow[x].alpha()))});
                    }
                }
            } else if (displayValidationError.isEmpty()) {
                displayValidationError = !gpuError.isEmpty() ? gpuError : cpuError;
            }
        }
    }
    bool displayGamutBranchPassed = false;
    DisplayGpuLutData gamutProbe;
    gamutProbe.edgeSize = 2;
    gamutProbe.gamutWarning = true;
    gamutProbe.gamutWarningThreshold = 14.0f;
    gamutProbe.forwardRgba16f.resize(gamutProbe.texelCount() * 4);
    gamutProbe.gamutRoundTripRgba16f.resize(gamutProbe.texelCount() * 4);
    for (int green = 0; green < gamutProbe.edgeSize; ++green) {
        for (int blue = 0; blue < gamutProbe.edgeSize; ++blue) {
            for (int red = 0; red < gamutProbe.edgeSize; ++red) {
                const qsizetype texel = qsizetype(red)
                    + qsizetype(blue) * gamutProbe.edgeSize
                    + qsizetype(green) * gamutProbe.edgeSize * gamutProbe.edgeSize;
                const qsizetype offset = texel * 4;
                const qfloat16 r(static_cast<float>(red));
                const qfloat16 g(static_cast<float>(green));
                const qfloat16 b(static_cast<float>(blue));
                gamutProbe.forwardRgba16f[offset] = r;
                gamutProbe.forwardRgba16f[offset + 1] = g;
                gamutProbe.forwardRgba16f[offset + 2] = b;
                gamutProbe.forwardRgba16f[offset + 3] = qfloat16(1.0f);
                gamutProbe.gamutRoundTripRgba16f[offset] = r;
                gamutProbe.gamutRoundTripRgba16f[offset + 1] = g;
                gamutProbe.gamutRoundTripRgba16f[offset + 2] = b;
                gamutProbe.gamutRoundTripRgba16f[offset + 3] = qfloat16(1.0f);
            }
        }
    }
    // Force only the pure-red lattice vertex out of gamut. This exercises the
    // second LUT and warning branch without adding another expensive ICC bake
    // to every diagnostic launch.
    const qsizetype pureRedOffset = qsizetype(1) * 4;
    gamutProbe.gamutRoundTripRgba16f[pureRedOffset] = qfloat16(0.0f);
    gamutProbe.fingerprint = QCryptographicHash::hash(
        QByteArrayLiteral("VFXPhotoLab/display-gamut-probe/v1"),
        QCryptographicHash::Sha256);
    QImage gamutSource(2, 1, QImage::Format_RGBA8888);
    uchar *gamutRow = gamutSource.scanLine(0);
    gamutRow[0] = 255; gamutRow[1] = 0; gamutRow[2] = 0; gamutRow[3] = 73;
    gamutRow[4] = 0; gamutRow[5] = 0; gamutRow[6] = 0; gamutRow[7] = 19;
    QString gamutError;
    const QImage gamutResult = applyDisplayColourTransform(
        gamutSource, gamutProbe, nullptr, &gamutError);
    if (!gamutResult.isNull()) {
        const uchar *resultRow = gamutResult.constScanLine(0);
        displayGamutBranchPassed = resultRow[0] == 255
            && resultRow[1] == 0
            && resultRow[2] == 255
            && resultRow[3] == 73
            && resultRow[4] == 0
            && resultRow[5] == 0
            && resultRow[6] == 0
            && resultRow[7] == 19;
    } else if (displayValidationError.isEmpty()) {
        displayValidationError = gamutError;
    }

    m_impl->displayTransformSelfTestPassed = displayDifference8 >= 0
        && displayDifference8 <= 2
        && displayDifferencePremultiplied >= 0
        && displayDifferencePremultiplied <= 3
        && displayDifference16 >= 0
        && displayDifference16 <= 384
        && displayGamutBranchPassed;
    if (m_impl->displayTransformSelfTestPassed) {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Display colour-transform WGSL parity passed (RGBA8 max difference %1; premultiplied RGBA8 %2; RGBA16 %3; gamut warning passed).")
                                  .arg(displayDifference8)
                                  .arg(displayDifferencePremultiplied)
                                  .arg(displayDifference16);
    } else {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Display colour-transform WGSL parity was not approved (RGBA8 %1; premultiplied RGBA8 %2; RGBA16 %3; gamut warning %4%5); presentation uses the CPU reference.")
                                  .arg(displayDifference8)
                                  .arg(displayDifferencePremultiplied)
                                  .arg(displayDifference16)
                                  .arg(displayGamutBranchPassed
                                           ? QStringLiteral("passed")
                                           : QStringLiteral("failed"))
                                  .arg(displayValidationError.isEmpty()
                                           ? QString()
                                           : QStringLiteral("; %1").arg(displayValidationError));
    }

    qInfo().noquote() << "[GPU diagnostic] Beginning tiled Fill application parity test.";
    QImage fillSource(37, 29, QImage::Format_RGBA8888);
    QImage fillCoverage(fillSource.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < fillSource.height(); ++y) {
        uchar *sourceRow = fillSource.scanLine(y);
        uchar *coverageRow = fillCoverage.scanLine(y);
        for (int x = 0; x < fillSource.width(); ++x) {
            sourceRow[x * 4] = static_cast<uchar>((17 + x * 9 + y * 5) & 255);
            sourceRow[x * 4 + 1] = static_cast<uchar>((31 + x * 3 + y * 13) & 255);
            sourceRow[x * 4 + 2] = static_cast<uchar>((73 + x * 11 + y * 7) & 255);
            sourceRow[x * 4 + 3] = static_cast<uchar>((x + y) % 7 == 0
                                                        ? 0
                                                        : ((47 + x * 5 + y * 17) & 255));
            coverageRow[x] = static_cast<uchar>((x * 19 + y * 23) & 255);
        }
    }
    QImage fillMaskSource(fillSource.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < fillMaskSource.height(); ++y) {
        uchar *row = fillMaskSource.scanLine(y);
        for (int x = 0; x < fillMaskSource.width(); ++x) {
            row[x] = static_cast<uchar>((29 + x * 7 + y * 13) & 255);
        }
    }
    const QColor fillValidationColour(211, 43, 137, 109);
    int fillDifference = 0;
    QStringList fillFailures;
    const auto validateFill = [&](const QString &name,
                                  const QImage &source,
                                  const FillTarget target,
                                  const int componentIndex,
                                  const bool preserveTransparency) {
        const FillApplyResult cpu = applyFillCoverageCpu(source,
                                                         fillCoverage,
                                                         target,
                                                         componentIndex,
                                                         fillValidationColour,
                                                         preserveTransparency);
        QString fillError;
        const QImage gpu = applyFillTile(source,
                                         fillCoverage,
                                         target,
                                         componentIndex,
                                         fillValidationColour,
                                         preserveTransparency,
                                         &fillError);
        if (!cpu.succeeded() || gpu.isNull()) {
            fillFailures.push_back(QStringLiteral("%1 unavailable (%2)")
                                       .arg(name,
                                            fillError.isEmpty() ? cpu.error : fillError));
            return;
        }
        const int difference = compareChannels(cpu.image, gpu).maximum;
        fillDifference = std::max(fillDifference, difference);
        if (difference > 1) {
            fillFailures.push_back(QStringLiteral("%1 differed by %2")
                                       .arg(name).arg(difference));
        }
    };
    validateFill(QStringLiteral("raster RGBA"), fillSource,
                 FillTarget::RasterPixels, -1, false);
    validateFill(QStringLiteral("raster preserve Alpha"), fillSource,
                 FillTarget::RasterPixels, -1, true);
    validateFill(QStringLiteral("greyscale channel"), fillSource,
                 FillTarget::GreyChannel, -1, false);
    validateFill(QStringLiteral("red channel"), fillSource,
                 FillTarget::ComponentChannel, 0, false);
    validateFill(QStringLiteral("Alpha channel"), fillSource,
                 FillTarget::ComponentChannel, 3, false);
    validateFill(QStringLiteral("mask"), fillMaskSource,
                 FillTarget::Mask, -1, false);
    m_impl->fillSelfTestPassed = fillFailures.isEmpty();
    if (m_impl->fillSelfTestPassed) {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Tiled Fill application parity passed for raster, transparency, channel and mask modes (max difference %1).")
                                  .arg(fillDifference);
    } else {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Tiled Fill application parity failed for %1; Fill will use the exact CPU fallback.")
                                  .arg(fillFailures.join(QStringLiteral(", ")));
    }

    qInfo().noquote() << "[GPU diagnostic] Beginning tiled Gradient application parity test.";
    int gradientDifference = 0;
    QStringList gradientFailures;
    const QPointF gradientStart(5.25, 7.5);
    const QPointF gradientEnd(31.0, 22.75);
    const QColor gradientStartColour(19, 177, 231, 213);
    const QColor gradientEndColour(241, 37, 83, 41);
    const auto validateGradient = [&](const QString &name,
                                      const QImage &source,
                                      const FillTarget target,
                                      const int componentIndex,
                                      const RasterGradientType type,
                                      const QColor &startColour,
                                      const QColor &endColour,
                                      const bool reverse) {
        GradientApplyRequest request;
        request.sourceImage = source;
        request.selectionCoverage = fillCoverage;
        request.target = target;
        request.componentIndex = componentIndex;
        request.start = gradientStart;
        request.end = gradientEnd;
        request.type = type;
        request.startColour = startColour;
        request.endColour = endColour;
        request.reverse = reverse;
        const GradientApplyResult cpu = applyGradientCpu(request);
        QString gradientError;
        const QImage gpu = applyGradientTile(source,
                                             fillCoverage,
                                             QPoint(),
                                             target,
                                             componentIndex,
                                             gradientStart,
                                             gradientEnd,
                                             type,
                                             startColour,
                                             endColour,
                                             reverse,
                                             &gradientError);
        if (!cpu.succeeded() || gpu.isNull()) {
            gradientFailures.push_back(QStringLiteral("%1 unavailable (%2)")
                                           .arg(name,
                                                gradientError.isEmpty()
                                                    ? cpu.error : gradientError));
            return;
        }
        const int difference = compareChannels(cpu.image, gpu).maximum;
        gradientDifference = std::max(gradientDifference, difference);
        if (difference > 1) {
            gradientFailures.push_back(QStringLiteral("%1 differed by %2")
                                           .arg(name).arg(difference));
        }
    };
    validateGradient(QStringLiteral("linear RGBA"), fillSource,
                     FillTarget::RasterPixels, -1,
                     RasterGradientType::Linear,
                     gradientStartColour, gradientEndColour, false);
    validateGradient(QStringLiteral("radial mask"), fillMaskSource,
                     FillTarget::Mask, -1,
                     RasterGradientType::Radial,
                     gradientStartColour, gradientEndColour, false);
    validateGradient(QStringLiteral("angle red channel"), fillSource,
                     FillTarget::ComponentChannel, 0,
                     RasterGradientType::Angle,
                     gradientStartColour, gradientEndColour, true);
    validateGradient(QStringLiteral("reflected greyscale"), fillSource,
                     FillTarget::GreyChannel, -1,
                     RasterGradientType::Reflected,
                     gradientStartColour, gradientEndColour, false);
    validateGradient(QStringLiteral("diamond Alpha channel"), fillSource,
                     FillTarget::ComponentChannel, 3,
                     RasterGradientType::Diamond,
                     gradientStartColour, gradientEndColour, false);
    validateGradient(QStringLiteral("transparent low-Alpha RGBA"), fillSource,
                     FillTarget::RasterPixels, -1,
                     RasterGradientType::Linear,
                     QColor(203, 91, 37, 3), QColor(203, 91, 37, 0), false);
    m_impl->gradientSelfTestPassed = gradientFailures.isEmpty();
    if (m_impl->gradientSelfTestPassed) {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Tiled Gradient application parity passed for all five modes and raster/channel/mask targets (max difference %1).")
                                  .arg(gradientDifference);
    } else {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Tiled Gradient application parity failed for %1; Gradient will use the exact CPU fallback.")
                                  .arg(gradientFailures.join(QStringLiteral(", ")));
    }

    qInfo().noquote() << "[GPU diagnostic] Beginning vector Feather coverage parity test.";
    int vectorFeatherDifference = -1;
    QString vectorFeatherError;
    VectorFeatherGpuTileData featherPrepared;
    featherPrepared.sourceRect = QRect(-7, -5, 39, 35);
    featherPrepared.outputRect = QRect(1, 2, 23, 19);
    featherPrepared.radiusX = 4.5;
    featherPrepared.radiusY = 3.25;
    featherPrepared.coverage = QImage(
        featherPrepared.sourceRect.size(), QImage::Format_RGBA8888);
    featherPrepared.colourCarrier = QImage(
        featherPrepared.outputRect.size(), QImage::Format_RGBA8888);
    featherPrepared.coverage.fill(Qt::transparent);
    featherPrepared.colourCarrier.fill(Qt::transparent);
    for (int y = 0; y < featherPrepared.coverage.height(); ++y) {
        uchar *row = featherPrepared.coverage.scanLine(y);
        for (int x = 0; x < featherPrepared.coverage.width(); ++x) {
            const int gx = featherPrepared.sourceRect.x() + x;
            const int gy = featherPrepared.sourceRect.y() + y;
            int alpha = 0;
            if (gx >= -2 && gx <= 19 && gy >= 0 && gy <= 22) alpha = 255;
            if (gx >= 5 && gx <= 10 && gy >= 7 && gy <= 14) alpha = 0;
            if ((gx + gy) % 7 == 0 && alpha > 0) alpha = 143;
            row[x * 4] = 255;
            row[x * 4 + 1] = 255;
            row[x * 4 + 2] = 255;
            row[x * 4 + 3] = static_cast<uchar>(alpha);
        }
    }
    for (int y = 0; y < featherPrepared.colourCarrier.height(); ++y) {
        uchar *row = featherPrepared.colourCarrier.scanLine(y);
        for (int x = 0; x < featherPrepared.colourCarrier.width(); ++x) {
            row[x * 4] = static_cast<uchar>((29 + x * 17 + y * 3) & 255);
            row[x * 4 + 1] = static_cast<uchar>((71 + x * 5 + y * 19) & 255);
            row[x * 4 + 2] = static_cast<uchar>((113 + x * 11 + y * 7) & 255);
            row[x * 4 + 3] = static_cast<uchar>(64 + ((x * 13 + y * 23) % 192));
        }
    }
    const auto parityDiscreteKernel = [](const int support) {
        QVector<double> values {1.0};
        const int base = support / 3;
        const int remainder = support % 3;
        for (int pass = 0; pass < 3; ++pass) {
            const int radius = base + (pass < remainder ? 1 : 0);
            const int width = radius * 2 + 1;
            QVector<double> next(values.size() + width - 1, 0.0);
            for (qsizetype index = 0; index < values.size(); ++index) {
                const double contribution = values[index] / width;
                for (int tap = 0; tap < width; ++tap) {
                    next[index + tap] += contribution;
                }
            }
            values = std::move(next);
        }
        return values;
    };
    const auto parityFractionalKernel = [&](const double radius) {
        const int lowSupport = static_cast<int>(std::floor(radius));
        const int highSupport = static_cast<int>(std::ceil(radius));
        const double blend = std::clamp(radius - std::floor(radius), 0.0, 1.0);
        const QVector<double> low = parityDiscreteKernel(lowSupport);
        const QVector<double> high = parityDiscreteKernel(highSupport);
        QVector<double> result(highSupport * 2 + 1, 0.0);
        const int lowOffset = highSupport - lowSupport;
        for (qsizetype index = 0; index < low.size(); ++index) {
            result[lowOffset + index] += low[index] * (1.0 - blend);
        }
        for (qsizetype index = 0; index < high.size(); ++index) {
            result[index] += high[index] * blend;
        }
        return result;
    };
    const QVector<double> parityKernelX = parityFractionalKernel(
        featherPrepared.radiusX);
    const QVector<double> parityKernelY = parityFractionalKernel(
        featherPrepared.radiusY);
    const int paritySupportX = parityKernelX.size() / 2;
    const int paritySupportY = parityKernelY.size() / 2;
    QVector<double> parityHorizontal(
        static_cast<qsizetype>(featherPrepared.coverage.height())
            * featherPrepared.colourCarrier.width(),
        0.0);
    for (int sy = 0; sy < featherPrepared.coverage.height(); ++sy) {
        for (int ox = 0; ox < featherPrepared.colourCarrier.width(); ++ox) {
            const int globalX = featherPrepared.outputRect.x() + ox;
            const int centreX = globalX - featherPrepared.sourceRect.x();
            double sum = 0.0;
            for (int offset = -paritySupportX; offset <= paritySupportX; ++offset) {
                const int sx = centreX + offset;
                if (sx < 0 || sx >= featherPrepared.coverage.width()) continue;
                sum += (featherPrepared.coverage.constScanLine(sy)[sx * 4 + 3]
                        / 255.0)
                    * parityKernelX[offset + paritySupportX];
            }
            parityHorizontal[static_cast<qsizetype>(sy)
                                 * featherPrepared.colourCarrier.width() + ox] = sum;
        }
    }
    QImage vectorFeatherCpu(
        featherPrepared.colourCarrier.size(), QImage::Format_RGBA8888);
    vectorFeatherCpu.fill(Qt::transparent);
    for (int oy = 0; oy < vectorFeatherCpu.height(); ++oy) {
        uchar *target = vectorFeatherCpu.scanLine(oy);
        const uchar *carrier = featherPrepared.colourCarrier.constScanLine(oy);
        const int globalY = featherPrepared.outputRect.y() + oy;
        const int centreY = globalY - featherPrepared.sourceRect.y();
        for (int ox = 0; ox < vectorFeatherCpu.width(); ++ox) {
            double sum = 0.0;
            for (int offset = -paritySupportY; offset <= paritySupportY; ++offset) {
                const int sy = centreY + offset;
                if (sy < 0 || sy >= featherPrepared.coverage.height()) continue;
                sum += parityHorizontal[static_cast<qsizetype>(sy)
                                            * vectorFeatherCpu.width() + ox]
                    * parityKernelY[offset + paritySupportY];
            }
            target[ox * 4] = carrier[ox * 4];
            target[ox * 4 + 1] = carrier[ox * 4 + 1];
            target[ox * 4 + 2] = carrier[ox * 4 + 2];
            target[ox * 4 + 3] = static_cast<uchar>(std::lround(
                std::clamp(sum * (carrier[ox * 4 + 3] / 255.0), 0.0, 1.0)
                * 255.0));
        }
    }
    const QImage vectorFeatherGpu = featherVectorCoverageTile(
        featherPrepared, &vectorFeatherError);
    if (!vectorFeatherGpu.isNull()) {
        vectorFeatherDifference = compareChannels(
            vectorFeatherCpu, vectorFeatherGpu).maximum;
    }
    m_impl->vectorFeatherSelfTestPassed = vectorFeatherDifference >= 0
        && vectorFeatherDifference <= 1;
    if (m_impl->vectorFeatherSelfTestPassed) {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Vector Feather coverage parity passed "
            "(maximum difference %1).")
                                  .arg(vectorFeatherDifference);
    } else {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Vector Feather coverage parity failed (%1); "
            "non-zero vector Feather will use the exact CPU fallback.")
                                  .arg(vectorFeatherError.isEmpty()
                                           ? QStringLiteral("difference %1")
                                                 .arg(vectorFeatherDifference)
                                           : vectorFeatherError);
    }

    qInfo().noquote() << "[GPU diagnostic] Beginning WGSL adjustment parity test.";
    QImage adjustmentSource(32, 32, QImage::Format_RGBA8888);
    for (int y = 0; y < adjustmentSource.height(); ++y) {
        for (int x = 0; x < adjustmentSource.width(); ++x) {
            adjustmentSource.setPixelColor(x,
                                           y,
                                           QColor((19 + x * 7 + y * 3) & 255,
                                                  (37 + x * 2 + y * 11) & 255,
                                                  (71 + x * 13 + y * 5) & 255,
                                                  255));
        }
    }
    QImage adjustmentMask(adjustmentSource.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < adjustmentMask.height(); ++y) {
        uchar *row = adjustmentMask.scanLine(y);
        for (int x = 0; x < adjustmentMask.width(); ++x) {
            row[x] = static_cast<uchar>((x * 5 + y * 3 + 64) & 255);
        }
    }

    LayerNode base;
    base.type = LayerType::BaseImage;

    LayerNode exposure;
    exposure.type = LayerType::Adjustment;
    ExposureParameters validationExposure;
    validationExposure.exposure = 0.75;
    validationExposure.offset = 0.012;
    validationExposure.gamma = 1.08;
    exposure.setExposureParameters(validationExposure);
    exposure.opacity = 0.8;
    exposure.maskImage = adjustmentMask;

    LayerNode contrast;
    contrast.type = LayerType::Adjustment;
    ContrastParameters validationContrast;
    validationContrast.contrast = 34.0;
    validationContrast.pivot = 0.43;
    contrast.setContrastParameters(validationContrast);
    contrast.opacity = 0.65;
    contrast.blendMode = BlendMode::Overlay;

    LayerNode saturation;
    saturation.type = LayerType::Adjustment;
    SaturationParameters validationSaturation;
    validationSaturation.saturation = 88.0;
    saturation.setSaturationParameters(validationSaturation);
    saturation.opacity = 0.9;

    LayerNode levels;
    levels.type = LayerType::Adjustment;
    LevelsParameters validationLevels;
    auto &validationMaster = validationLevels.channel(AdjustmentChannel::Rgb);
    validationMaster.inputBlack = 0.08;
    validationMaster.inputWhite = 0.91;
    validationMaster.gamma = 1.3;
    validationMaster.outputBlack = 0.03;
    validationMaster.outputWhite = 0.96;
    auto &validationRed = validationLevels.channel(AdjustmentChannel::Red);
    validationRed.inputBlack = 0.04;
    validationRed.inputWhite = 0.89;
    validationRed.gamma = 0.84;
    validationRed.outputBlack = 0.06;
    validationRed.outputWhite = 0.87;
    auto &validationGreen = validationLevels.channel(AdjustmentChannel::Green);
    validationGreen.gamma = 1.22;
    validationGreen.outputBlack = 0.02;
    validationGreen.outputWhite = 0.93;
    auto &validationBlue = validationLevels.channel(AdjustmentChannel::Blue);
    validationBlue.inputBlack = 0.1;
    validationBlue.inputWhite = 0.97;
    validationBlue.outputBlack = 0.12;
    validationBlue.outputWhite = 0.99;
    levels.setLevelsParameters(validationLevels);
    levels.opacity = 0.7;


    LayerNode curves;
    curves.type = LayerType::Adjustment;
    CurvesParameters validationCurves;
    validationCurves.interpolation = CurveInterpolation::Smooth;
    validationCurves.channel(AdjustmentChannel::Rgb).points = {
        {0.0, 0.0}, {0.22, 0.14}, {0.56, 0.69}, {1.0, 1.0}
    };
    validationCurves.channel(AdjustmentChannel::Red).points = {
        {0.0, 0.03}, {0.48, 0.57}, {1.0, 0.96}
    };
    validationCurves.channel(AdjustmentChannel::Blue).points = {
        {0.0, 0.0}, {0.35, 0.29}, {0.8, 0.9}, {1.0, 1.0}
    };
    validationCurves.normalise();
    curves.setCurvesParameters(validationCurves);
    curves.opacity = 0.82;

    LayerNode hueSaturation;
    hueSaturation.type = LayerType::Adjustment;
    HueSaturationParameters validationHueSaturation;
    validationHueSaturation.hue = 11.0;
    validationHueSaturation.saturation = 18.0;
    validationHueSaturation.lightness = -4.0;
    auto &validationReds = validationHueSaturation.range(HueSaturationRange::Reds);
    validationReds.hue = -17.0;
    validationReds.saturation = 32.0;
    validationReds.lightness = 7.0;
    validationReds.width = 48.0;
    validationReds.feather = 24.0;
    auto &validationBlues = validationHueSaturation.range(HueSaturationRange::Blues);
    validationBlues.hue = 21.0;
    validationBlues.saturation = -26.0;
    validationHueSaturation.normalise();
    hueSaturation.setHueSaturationParameters(validationHueSaturation);
    hueSaturation.opacity = 0.73;

    LayerNode vibrance;
    vibrance.type = LayerType::Adjustment;
    VibranceParameters validationVibrance;
    validationVibrance.vibrance = 58.0;
    validationVibrance.saturation = -9.0;
    validationVibrance.skinProtection = 72.0;
    vibrance.setVibranceParameters(validationVibrance);
    vibrance.opacity = 0.77;

    LayerNode whiteBalance;
    whiteBalance.type = LayerType::Adjustment;
    WhiteBalanceParameters validationWhiteBalance;
    validationWhiteBalance.temperature = 24.0;
    validationWhiteBalance.tint = -13.0;
    whiteBalance.setWhiteBalanceParameters(validationWhiteBalance);
    whiteBalance.opacity = 0.69;

    LayerNode colourBalance;
    colourBalance.type = LayerType::Adjustment;
    ColourBalanceParameters validationColourBalance;
    validationColourBalance.range(ColourBalanceRange::Shadows).cyanRed = -18.0;
    validationColourBalance.range(ColourBalanceRange::Shadows).yellowBlue = 13.0;
    validationColourBalance.range(ColourBalanceRange::Midtones).magentaGreen = -11.0;
    validationColourBalance.range(ColourBalanceRange::Highlights).cyanRed = 16.0;
    validationColourBalance.range(ColourBalanceRange::Highlights).yellowBlue = -14.0;
    validationColourBalance.preserveLuminosity = true;
    colourBalance.setColourBalanceParameters(validationColourBalance);
    colourBalance.opacity = 0.74;

    LayerNode channelMixer;
    channelMixer.type = LayerType::Adjustment;
    ChannelMixerParameters validationChannelMixer;
    validationChannelMixer.output(ChannelMixerOutput::Red) = {112.0, -8.0, 4.0, -3.0};
    validationChannelMixer.output(ChannelMixerOutput::Green) = {-6.0, 104.0, 7.0, 2.0};
    validationChannelMixer.output(ChannelMixerOutput::Blue) = {3.0, 9.0, 91.0, 1.0};
    channelMixer.setChannelMixerParameters(validationChannelMixer);
    channelMixer.opacity = 0.71;

    LayerNode blackAndWhite;
    blackAndWhite.type = LayerType::Adjustment;
    BlackAndWhiteParameters validationBlackAndWhite;
    validationBlackAndWhite.colourWeights = {145.0, 78.0, 116.0, 67.0, 132.0, 88.0};
    validationBlackAndWhite.tintEnabled = true;
    validationBlackAndWhite.tintHue = 38.0;
    validationBlackAndWhite.tintSaturation = 24.0;
    blackAndWhite.setBlackAndWhiteParameters(validationBlackAndWhite);
    blackAndWhite.opacity = 0.68;

    LayerNode selectiveColour;
    selectiveColour.type = LayerType::Adjustment;
    SelectiveColourParameters validationSelectiveColour;
    validationSelectiveColour.range(SelectiveColourRange::Reds) = {18.0, -9.0, 12.0, 4.0};
    validationSelectiveColour.range(SelectiveColourRange::Blues) = {-11.0, 16.0, -7.0, 8.0};
    validationSelectiveColour.range(SelectiveColourRange::Whites).black = -6.0;
    validationSelectiveColour.range(SelectiveColourRange::Neutrals) = {3.0, -5.0, 7.0, 4.0};
    validationSelectiveColour.range(SelectiveColourRange::Blacks) = {-4.0, 6.0, 2.0, 11.0};
    validationSelectiveColour.method = SelectiveColourMethod::Relative;
    selectiveColour.setSelectiveColourParameters(validationSelectiveColour);
    selectiveColour.opacity = 0.72;

    LayerNode gradientMap;
    gradientMap.type = LayerType::Adjustment;
    GradientMapParameters validationGradientMap;
    validationGradientMap.stops = {{0.0, QColor(12, 20, 48)},
                                   {0.37, QColor(94, 42, 116)},
                                   {0.72, QColor(235, 136, 74)},
                                   {1.0, QColor(255, 244, 205)}};
    validationGradientMap.interpolation = GradientInterpolation::Smooth;
    gradientMap.setGradientMapParameters(validationGradientMap);
    gradientMap.opacity = 0.76;

    LayerNode posterise;
    posterise.type = LayerType::Adjustment;
    PosteriseParameters validationPosterise;
    validationPosterise.levels = 7;
    posterise.setPosteriseParameters(validationPosterise);
    posterise.opacity = 0.81;

    LayerNode threshold;
    threshold.type = LayerType::Adjustment;
    ThresholdParameters validationThreshold;
    validationThreshold.threshold = 109.0 / 255.0;
    validationThreshold.source = ThresholdSource::Blue;
    threshold.setThresholdParameters(validationThreshold);
    threshold.opacity = 0.67;

    LayerNode invert;
    invert.type = LayerType::Adjustment;
    invert.setInvertParameters();
    invert.opacity = 0.71;

    LayerNode photoFilter;
    photoFilter.type = LayerType::Adjustment;
    PhotoFilterParameters validationPhotoFilter;
    validationPhotoFilter.colour = QColor(57, 126, 242);
    validationPhotoFilter.density = 43.0;
    validationPhotoFilter.preserveLuminosity = true;
    photoFilter.setPhotoFilterParameters(validationPhotoFilter);
    photoFilter.opacity = 0.78;

    LayerNode lut;
    lut.type = LayerType::Adjustment;
    LutParameters validationLut;
    validationLut.title = QStringLiteral("GPU parity LUT");
    validationLut.shaperSize = 3;
    validationLut.shaperData = {0.0f, 0.0f, 0.0f,
                                102.0f / 255.0f, 128.0f / 255.0f, 153.0f / 255.0f,
                                1.0f, 1.0f, 1.0f};
    validationLut.cubeSize = 2;
    validationLut.cubeData = {
        0.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,   1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,   1.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 1.0f,   1.0f, 1.0f, 1.0f
    };
    // Exercise the 0.10.1f authoritative floating-point tetrahedral path.
    // The table contains fractional values that cannot round-trip through the
    // former RGBA8 lookup transport.
    validationLut.interpolation = LutInterpolation::Tetrahedral;
    validationLut.strength = 73.0;
    validationLut.normalise();
    lut.setLutParameters(validationLut);
    lut.opacity = 0.79;

    LayerNode lutTrilinear = lut;
    LutParameters validationTrilinearLut = validationLut;
    validationTrilinearLut.interpolation = LutInterpolation::Trilinear;
    validationTrilinearLut.normalise();
    lutTrilinear.setLutParameters(validationTrilinearLut);

    LayerNode lutLinearProcessing;
    lutLinearProcessing.type = LayerType::Adjustment;
    LutParameters validationLinearProcessing;
    validationLinearProcessing.title = QStringLiteral("GPU linear-processing parity LUT");
    validationLinearProcessing.shaperSize = 2;
    validationLinearProcessing.shaperData = {1.0f, 1.0f, 1.0f,
                                             0.0f, 0.0f, 0.0f};
    validationLinearProcessing.processingMode = LutProcessingMode::LinearSrgb;
    validationLinearProcessing.interpolation = LutInterpolation::Trilinear;
    validationLinearProcessing.strength = 61.0;
    validationLinearProcessing.normalise();
    lutLinearProcessing.setLutParameters(validationLinearProcessing);
    lutLinearProcessing.opacity = 0.82;

    const QVector<float> identityCube {
        0.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,   1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,   1.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 1.0f,   1.0f, 1.0f, 1.0f
    };
    LayerNode lutTony;
    lutTony.type = LayerType::Adjustment;
    LutParameters validationTony;
    validationTony.title = QStringLiteral("GPU Tony McMapface parity LUT");
    validationTony.cubeSize = 2;
    validationTony.cubeData = identityCube;
    validationTony.operatorProfile = LutOperatorProfile::TonyMcMapface;
    validationTony.strength = 69.0;
    validationTony.normalise();
    lutTony.setLutParameters(validationTony);
    lutTony.opacity = 0.77;

    LayerNode lutAgX;
    lutAgX.type = LayerType::Adjustment;
    LutParameters validationAgX = validationTony;
    validationAgX.title = QStringLiteral("GPU AgX Base sRGB parity LUT");
    validationAgX.operatorProfile = LutOperatorProfile::AgXBaseSrgb;
    validationAgX.strength = 64.0;
    validationAgX.tableFingerprint = 0;
    validationAgX.normalise();
    lutAgX.setLutParameters(validationAgX);
    lutAgX.opacity = 0.74;

    LayerNode shadowsHighlights;
    shadowsHighlights.type = LayerType::Adjustment;
    ShadowsHighlightsParameters validationShadowsHighlights;
    validationShadowsHighlights.shadowAmount = 42.0;
    validationShadowsHighlights.shadowTonalWidth = 58.0;
    validationShadowsHighlights.highlightAmount = 31.0;
    validationShadowsHighlights.highlightTonalWidth = 47.0;
    validationShadowsHighlights.radius = 11.0;
    validationShadowsHighlights.midtoneContrast = 14.0;
    validationShadowsHighlights.colourCorrection = 28.0;
    shadowsHighlights.setShadowsHighlightsParameters(validationShadowsHighlights);
    shadowsHighlights.opacity = 0.83;

    const auto preparedAdjustment = [&](const LayerNode &layer) {
        PreparedTileLayer prepared;
        prepared.kind = PreparedTileLayer::Kind::Adjustment;
        prepared.mask = layer.maskImage;
        prepared.opacity = layer.opacity;
        prepared.blendMode = layer.blendMode;
        const AdjustmentData data = layer.effectiveAdjustmentData();
        prepared.adjustmentType = data.type;
        if (data.type == AdjustmentType::Exposure) {
            prepared.exposureParameters = std::get<ExposureParameters>(data.parameters);
            prepared.exposure = prepared.exposureParameters.exposure;
        } else if (data.type == AdjustmentType::Contrast) {
            prepared.contrastParameters = std::get<ContrastParameters>(data.parameters);
            prepared.contrast = prepared.contrastParameters.contrast;
        } else if (data.type == AdjustmentType::Saturation) {
            prepared.saturationParameters = std::get<SaturationParameters>(data.parameters);
            prepared.saturation = prepared.saturationParameters.saturation;
        } else if (data.type == AdjustmentType::Levels) {
            prepared.levels = std::get<LevelsParameters>(data.parameters);
            prepared.tonalLookup = buildTonalLookup(data, 8).toRgba8Image();
        } else if (data.type == AdjustmentType::Curves) {
            prepared.curves = std::get<CurvesParameters>(data.parameters);
            prepared.tonalLookup = buildTonalLookup(data, 8).toRgba8Image();
        } else if (data.type == AdjustmentType::HueSaturation) {
            prepared.hueSaturationParameters = std::get<HueSaturationParameters>(data.parameters);
        } else if (data.type == AdjustmentType::Vibrance) {
            prepared.vibranceParameters = std::get<VibranceParameters>(data.parameters);
        } else if (data.type == AdjustmentType::WhiteBalance) {
            prepared.whiteBalanceParameters = std::get<WhiteBalanceParameters>(data.parameters);
        } else if (data.type == AdjustmentType::ColourBalance) {
            prepared.colourBalanceParameters = std::get<ColourBalanceParameters>(data.parameters);
        } else if (data.type == AdjustmentType::ChannelMixer) {
            prepared.channelMixerParameters = std::get<ChannelMixerParameters>(data.parameters);
        } else if (data.type == AdjustmentType::BlackAndWhite) {
            prepared.blackAndWhiteParameters = std::get<BlackAndWhiteParameters>(data.parameters);
        } else if (data.type == AdjustmentType::GradientMap) {
            prepared.gradientMapParameters = std::get<GradientMapParameters>(data.parameters);
            prepared.tonalLookup = buildTonalLookup(data, 8).toRgba8Image();
        } else if (data.type == AdjustmentType::Posterise) {
            prepared.posteriseParameters = std::get<PosteriseParameters>(data.parameters);
        } else if (data.type == AdjustmentType::Threshold) {
            prepared.thresholdParameters = std::get<ThresholdParameters>(data.parameters);
        } else if (data.type == AdjustmentType::Invert) {
            prepared.invertParameters = std::get<InvertParameters>(data.parameters);
        } else if (data.type == AdjustmentType::PhotoFilter) {
            prepared.photoFilterParameters = std::get<PhotoFilterParameters>(data.parameters);
        } else if (data.type == AdjustmentType::SelectiveColour) {
            prepared.selectiveColourParameters = std::get<SelectiveColourParameters>(data.parameters);
        } else if (data.type == AdjustmentType::Lut) {
            prepared.lutParameters = std::get<LutParameters>(data.parameters);
            prepared.lutDocumentTransfer = CubeLut::documentTransferFor(
                adjustmentSource.colorSpace());
            prepared.lutLookup = CubeLut::buildGpuTextureData(
                prepared.lutParameters);
        } else if (data.type == AdjustmentType::ShadowsHighlights) {
            prepared.shadowsHighlightsParameters = std::get<ShadowsHighlightsParameters>(data.parameters);
        }
        return prepared;
    };

    PreparedTileLayer preparedBase;
    preparedBase.kind = PreparedTileLayer::Kind::Image;
    preparedBase.image = adjustmentSource;

    // Exercise the exact fast path used when a newly opened document contains
    // only one fully opaque raster layer. That layer may become the final
    // accumulator directly, so its uploaded texture must be COPY_SRC-capable
    // for the viewport readback.
    error.clear();
    const QImage singleRasterGpu = compositeHierarchyTile(adjustmentSource.size(),
                                                           adjustmentSource.colorSpace(),
                                                           {preparedBase},
                                                           &error);
    if (singleRasterGpu.isNull()) {
        if (details) {
            *details = QStringLiteral("Single-raster hierarchy readback failed: %1")
                           .arg(error.isEmpty() ? QStringLiteral("unknown GPU error") : error);
        }
        return false;
    }
    const int singleRasterDifference = compareChannels(adjustmentSource, singleRasterGpu).maximum;
    if (singleRasterDifference != 0) {
        if (details) {
            *details = QStringLiteral("Single-raster hierarchy readback parity failed (max difference %1)")
                           .arg(singleRasterDifference);
        }
        return false;
    }
    qInfo().noquote() << QStringLiteral(
        "[GPU diagnostic] Single-raster hierarchy readback passed (max difference %1).")
                             .arg(singleRasterDifference);

    struct AdjustmentParityCase {
        const LayerNode *layer = nullptr;
        QString name;
    };
    const QVector<AdjustmentParityCase> adjustmentCases {
        {&exposure, QStringLiteral("Exposure")},
        {&contrast, QStringLiteral("Contrast")},
        {&saturation, QStringLiteral("Saturation")},
        {&levels, QStringLiteral("Levels")},
        {&curves, QStringLiteral("Curves")},
        {&hueSaturation, QStringLiteral("Hue/Saturation")},
        {&vibrance, QStringLiteral("Vibrance")},
        {&whiteBalance, QStringLiteral("White Balance")},
        {&colourBalance, QStringLiteral("Colour Balance")},
        {&channelMixer, QStringLiteral("Channel Mixer")},
        {&blackAndWhite, QStringLiteral("Black and White")},
        {&selectiveColour, QStringLiteral("Selective Colour")},
        {&gradientMap, QStringLiteral("Gradient Map")},
        {&posterise, QStringLiteral("Posterise")},
        {&threshold, QStringLiteral("Threshold")},
        {&invert, QStringLiteral("Invert")},
        {&photoFilter, QStringLiteral("Photo Filter")},
        {&lut, QStringLiteral("LUT tetrahedral RGBA16Float")},
        {&lutTrilinear, QStringLiteral("LUT trilinear RGBA16Float")},
        {&lutLinearProcessing, QStringLiteral("LUT linear processing")},
        {&lutTony, QStringLiteral("LUT Tony McMapface")},
        {&lutAgX, QStringLiteral("LUT AgX Base sRGB")},
        {&shadowsHighlights, QStringLiteral("Shadows/Highlights")}
    };

    m_impl->approvedAdjustmentMask = 0;
    quint32 failedAdjustmentMask = 0;
    QStringList approvedAdjustments;
    QStringList rejectedAdjustments;
    int maximumAdjustmentDifference = 0;
    for (const AdjustmentParityCase &testCase : adjustmentCases) {
        const QVector<LayerNode> cpuLayers {*testCase.layer, base};
        const QImage cpuAdjusted = ImageProcessor::renderRegion(adjustmentSource,
                                                                 cpuLayers,
                                                                 adjustmentSource.rect(),
                                                                 adjustmentSource.size());
        const QVector<PreparedTileLayer> gpuLayers {
            preparedAdjustment(*testCase.layer), preparedBase
        };
        error.clear();
        const QImage gpuAdjusted = compositeHierarchyTile(adjustmentSource.size(),
                                                           adjustmentSource.colorSpace(),
                                                           gpuLayers,
                                                           &error);
        if (cpuAdjusted.isNull() || gpuAdjusted.isNull()) {
            rejectedAdjustments.push_back(
                error.isEmpty() ? testCase.name
                                : QStringLiteral("%1 (%2)").arg(testCase.name, error));
            failedAdjustmentMask |= quint32(1)
                << static_cast<quint32>(testCase.layer->adjustmentType);
            qInfo().noquote() << QStringLiteral(
                "[GPU diagnostic] %1 WGSL validation could not execute; only this adjustment uses the CPU reference.")
                                     .arg(testCase.name);
            continue;
        }

        const ParityDifference report = compareChannels(cpuAdjusted, gpuAdjusted);
        maximumAdjustmentDifference = std::max(maximumAdjustmentDifference,
                                               report.maximum);
        if (report.maximum <= 2) {
            m_impl->approvedAdjustmentMask |=
                quint32(1) << static_cast<quint32>(testCase.layer->adjustmentType);
            approvedAdjustments.push_back(testCase.name);
            qInfo().noquote() << QStringLiteral(
                "[GPU diagnostic] %1 WGSL parity passed (max difference %2).")
                                     .arg(testCase.name)
                                     .arg(report.maximum);
        } else {
            failedAdjustmentMask |= quint32(1)
                << static_cast<quint32>(testCase.layer->adjustmentType);
            rejectedAdjustments.push_back(
                QStringLiteral("%1 (delta %2)").arg(testCase.name).arg(report.maximum));
            qInfo().noquote() << QStringLiteral(
                "[GPU diagnostic] %1 WGSL parity failed (max difference %2); only this adjustment uses the CPU reference.")
                                     .arg(testCase.name)
                                     .arg(report.maximum);
        }
    }
    m_impl->approvedAdjustmentMask &= ~failedAdjustmentMask;
    if ((failedAdjustmentMask
         & (quint32(1) << static_cast<quint32>(AdjustmentType::Lut))) != 0) {
        for (qsizetype index = approvedAdjustments.size(); index-- > 0;) {
            if (approvedAdjustments.at(index).startsWith(QStringLiteral("LUT"))) {
                approvedAdjustments.removeAt(index);
            }
        }
    }

    qInfo().noquote()
        << "[GPU diagnostic] Beginning managed adjustment-domain WGSL parity test.";
    QImage managedSource(37, 29, QImage::Format_RGBA8888);
    for (int y = 0; y < managedSource.height(); ++y) {
        uchar *row = managedSource.scanLine(y);
        for (int x = 0; x < managedSource.width(); ++x) {
            row[x * 4] = static_cast<uchar>((23 + x * 17 + y * 5) & 255);
            row[x * 4 + 1] = static_cast<uchar>((61 + x * 3 + y * 19) & 255);
            row[x * 4 + 2] = static_cast<uchar>((97 + x * 11 + y * 7) & 255);
            row[x * 4 + 3] = static_cast<uchar>((x * 13 + y * 29) & 255);
        }
    }
    managedSource.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    PreparedTileLayer managedPreparedBase;
    managedPreparedBase.kind = PreparedTileLayer::Kind::Image;
    managedPreparedBase.image = managedSource;

    // The exact CPU compositor currently crosses a premultiplied QImage while
    // returning from an alternate adjustment domain. That representation can
    // discard RGB beneath low or zero alpha even though VFX Photo Lab's
    // straight-RGBA contract deliberately preserves it. Use an opaque twin as
    // the CPU colour reference, then validate the GPU alpha independently.
    // This makes the diagnostic prove both colour-transform parity and hidden
    // RGB preservation instead of approving a premultiplication artefact.
    QImage managedOpaqueSource = managedSource;
    managedOpaqueSource.detach();
    for (int y = 0; y < managedOpaqueSource.height(); ++y) {
        uchar *row = managedOpaqueSource.scanLine(y);
        for (int x = 0; x < managedOpaqueSource.width(); ++x) {
            row[x * 4 + 3] = 255;
        }
    }

    struct ManagedAdjustmentParityCase {
        LayerNode layer;
        QString name;
    };
    LayerNode managedExposure = exposure;
    managedExposure.opacity = 1.0;
    managedExposure.blendMode = BlendMode::Copy;
    managedExposure.maskImage = {};
    LayerNode managedSaturation = saturation;
    managedSaturation.opacity = 1.0;
    managedSaturation.blendMode = BlendMode::Copy;
    managedSaturation.maskImage = {};
    LayerNode managedPhotoFilter = photoFilter;
    managedPhotoFilter.opacity = 1.0;
    managedPhotoFilter.blendMode = BlendMode::Copy;
    managedPhotoFilter.maskImage = {};
    LayerNode managedSelectiveColour = selectiveColour;
    managedSelectiveColour.opacity = 1.0;
    managedSelectiveColour.blendMode = BlendMode::Copy;
    managedSelectiveColour.maskImage = {};
    const QVector<ManagedAdjustmentParityCase> managedCases {
        {managedExposure, QStringLiteral("Linear-working Exposure")},
        {managedSaturation, QStringLiteral("encoded-sRGB Saturation")},
        {managedPhotoFilter, QStringLiteral("encoded-sRGB Photo Filter")},
        {managedSelectiveColour, QStringLiteral("encoded-sRGB Selective Colour")}
    };
    QStringList managedAdjustmentFailures;
    int managedAdjustmentMaximumDifference = 0;
    for (const ManagedAdjustmentParityCase &testCase : managedCases) {
        const AdjustmentData data = testCase.layer.effectiveAdjustmentData();
        const AdjustmentProcessingDomain domain = adjustmentProcessingDomain(data);
        QString latticeError;
        const auto domainLut = createManagedAdjustmentGpuLut(
            managedSource.colorSpace(), domain, &latticeError);
        PreparedTileLayer prepared = preparedAdjustment(testCase.layer);
        prepared.processingDomain = domain;
        prepared.managedDomainLut = domainLut;
        const QVector<LayerNode> cpuLayers {testCase.layer, base};
        const QImage cpuManagedOpaque = ImageProcessor::renderRegion(
            managedOpaqueSource, cpuLayers, managedOpaqueSource.rect(),
            managedOpaqueSource.size(), nullptr,
            ColourProcessingCompatibility::ManagedV1);
        error.clear();
        const QImage gpuManaged = domainLut
            ? compositeHierarchyTile(managedSource.size(), managedSource.colorSpace(),
                                     {prepared, managedPreparedBase}, &error)
            : QImage();
        if (cpuManagedOpaque.isNull() || gpuManaged.isNull()) {
            managedAdjustmentFailures.push_back(
                QStringLiteral("%1 unavailable (%2)")
                    .arg(testCase.name,
                         !error.isEmpty() ? error
                                          : (!latticeError.isEmpty()
                                                 ? latticeError
                                                 : QStringLiteral("unknown error"))));
            continue;
        }
        const QImage cpuManagedRgba = cpuManagedOpaque.convertToFormat(
            QImage::Format_RGBA8888);
        const QImage gpuManagedRgba = gpuManaged.convertToFormat(
            QImage::Format_RGBA8888);
        int colourDifference = 0;
        int alphaDifference = 0;
        for (int y = 0; y < managedSource.height(); ++y) {
            const uchar *sourceRow = managedSource.constScanLine(y);
            const uchar *cpuRow = cpuManagedRgba.constScanLine(y);
            const uchar *gpuRow = gpuManagedRgba.constScanLine(y);
            for (int x = 0; x < managedSource.width(); ++x) {
                const int offset = x * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    colourDifference = std::max(
                        colourDifference,
                        std::abs(int(cpuRow[offset + channel])
                                 - int(gpuRow[offset + channel])));
                }
                alphaDifference = std::max(
                    alphaDifference,
                    std::abs(int(sourceRow[offset + 3])
                             - int(gpuRow[offset + 3])));
            }
        }
        const int difference = std::max(colourDifference, alphaDifference);
        managedAdjustmentMaximumDifference = std::max(
            managedAdjustmentMaximumDifference, difference);
        constexpr int ManagedAdjustmentColourParityLimit = 6;
        if (colourDifference > ManagedAdjustmentColourParityLimit
            || alphaDifference != 0) {
            managedAdjustmentFailures.push_back(
                QStringLiteral("%1 RGB/alpha differed by %2/%3 (limits %4/0)")
                    .arg(testCase.name)
                    .arg(colourDifference)
                    .arg(alphaDifference)
                    .arg(ManagedAdjustmentColourParityLimit));
        } else {
            qInfo().noquote() << QStringLiteral(
                "[GPU diagnostic] Managed %1 WGSL parity passed "
                "(RGB difference %2, alpha difference %3; hidden RGB tested).")
                                      .arg(testCase.name)
                                      .arg(colourDifference)
                                      .arg(alphaDifference);
        }
    }
    m_impl->managedAdjustmentTransformSelfTestPassed =
        managedAdjustmentFailures.isEmpty();
    if (!m_impl->managedAdjustmentTransformSelfTestPassed) {
        qInfo().noquote() << QStringLiteral(
            "[GPU diagnostic] Managed adjustment-domain WGSL parity was not approved for %1; managed adjustment stacks use the exact CPU reference.")
                                  .arg(managedAdjustmentFailures.join(
                                      QStringLiteral(", ")));
    }

    // A long chain is useful as a diagnostic, but cumulative one-code-value
    // rounding differences must not disable otherwise valid GPU features.
    const QVector<LayerNode> chainedCpuLayers {
        shadowsHighlights, lut, selectiveColour, photoFilter, invert, threshold,
        posterise, gradientMap, blackAndWhite, channelMixer, colourBalance,
        whiteBalance, vibrance, hueSaturation, curves, levels, saturation,
        contrast, exposure, base
    };
    const QImage chainedCpu = ImageProcessor::renderRegion(adjustmentSource,
                                                            chainedCpuLayers,
                                                            adjustmentSource.rect(),
                                                            adjustmentSource.size());
    const QVector<PreparedTileLayer> chainedGpuLayers {
        preparedAdjustment(shadowsHighlights), preparedAdjustment(lut),
        preparedAdjustment(selectiveColour), preparedAdjustment(photoFilter),
        preparedAdjustment(invert), preparedAdjustment(threshold),
        preparedAdjustment(posterise), preparedAdjustment(gradientMap),
        preparedAdjustment(blackAndWhite), preparedAdjustment(channelMixer),
        preparedAdjustment(colourBalance), preparedAdjustment(whiteBalance),
        preparedAdjustment(vibrance), preparedAdjustment(hueSaturation),
        preparedAdjustment(curves), preparedAdjustment(levels),
        preparedAdjustment(saturation), preparedAdjustment(contrast),
        preparedAdjustment(exposure), preparedBase
    };
    error.clear();
    const QImage chainedGpu = compositeHierarchyTile(adjustmentSource.size(),
                                                      adjustmentSource.colorSpace(),
                                                      chainedGpuLayers,
                                                      &error);
    const int chainedDifference = chainedCpu.isNull() || chainedGpu.isNull()
        ? -1 : compareChannels(chainedCpu, chainedGpu).maximum;
    qInfo().noquote() << (chainedDifference >= 0
        ? QStringLiteral("[GPU diagnostic] Combined sixteen-adjustment chain max difference %1 (informational only).")
              .arg(chainedDifference)
        : QStringLiteral("[GPU diagnostic] Combined adjustment-chain diagnostic unavailable: %1")
              .arg(error));

    // Identity texture transfer and tiled resize are the GPU foundation gate.
    // Adjustment approval is deliberately feature-specific so one driver
    // disagreement cannot force unrelated adjustments or compositing to CPU.
    m_impl->selfTestPassed = true;
    const QString approvedText = approvedAdjustments.isEmpty()
        ? QStringLiteral("none") : approvedAdjustments.join(QStringLiteral(", "));
    const QString rejectedText = rejectedAdjustments.isEmpty()
        ? QStringLiteral("none") : rejectedAdjustments.join(QStringLiteral(", "));
    m_impl->status = QStringLiteral(
        "Native WebGPU adapter/device/queue ready; texture parity passed (max difference %1), "
        "tiled Nearest/Bilinear resize parity passed (max difference %2). "
        "Feature-approved WGSL adjustments: %3. CPU-only adjustment fallbacks: %4. "
        "Maximum individual adjustment difference: %5; combined-chain diagnostic: %6. "
        "Tiled Fill application: %7. Tiled Gradient application: %8. "
        "Vector Feather coverage: %9. Display colour transforms: %10. "
        "Managed adjustment domains: %11.")
                         .arg(identityDifference)
                         .arg(resizeDifference)
                         .arg(approvedText)
                         .arg(rejectedText)
                         .arg(maximumAdjustmentDifference)
                         .arg(chainedDifference >= 0 ? QString::number(chainedDifference)
                                                     : QStringLiteral("unavailable"))
                         .arg(m_impl->fillSelfTestPassed
                                  ? QStringLiteral("GPU approved (max difference %1)").arg(fillDifference)
                                  : QStringLiteral("exact CPU fallback"))
                         .arg(m_impl->gradientSelfTestPassed
                                  ? QStringLiteral("GPU approved (max difference %1)").arg(gradientDifference)
                                  : QStringLiteral("exact CPU fallback"))
                         .arg(m_impl->vectorFeatherSelfTestPassed
                                  ? QStringLiteral("GPU approved (max difference %1)").arg(vectorFeatherDifference)
                                  : QStringLiteral("exact CPU fallback"))
                         .arg(m_impl->displayTransformSelfTestPassed
                                  ? QStringLiteral("GPU approved (RGBA8 %1, premultiplied %2, RGBA16 %3)")
                                        .arg(displayDifference8)
                                        .arg(displayDifferencePremultiplied)
                                        .arg(displayDifference16)
                                  : QStringLiteral("exact CPU fallback"))
                         .arg(m_impl->managedAdjustmentTransformSelfTestPassed
                                  ? QStringLiteral("GPU approved (max difference %1)")
                                        .arg(managedAdjustmentMaximumDifference)
                                  : QStringLiteral("exact CPU fallback"));
    if (details) *details = m_impl->status;
    qInfo().noquote() << "[GPU diagnostic]" << m_impl->status;
    return true;
}

WebGpuValidationState WebGpuContext::validationState() const
{
    std::lock_guard operationLock(m_impl->operationMutex);
    WebGpuValidationState state;
    state.foundation = m_impl->selfTestPassed;
    state.fill = m_impl->fillSelfTestPassed;
    state.gradient = m_impl->gradientSelfTestPassed;
    state.vectorFeather = m_impl->vectorFeatherSelfTestPassed;
    state.displayTransform = m_impl->displayTransformSelfTestPassed;
    state.managedAdjustmentTransforms =
        m_impl->managedAdjustmentTransformSelfTestPassed;
    state.approvedAdjustmentMask = m_impl->approvedAdjustmentMask;
    return state;
}

void WebGpuContext::adoptExternalValidationState(
    const WebGpuValidationState &state)
{
    std::lock_guard operationLock(m_impl->operationMutex);
    m_impl->selfTestPassed = state.foundation;
    m_impl->fillSelfTestPassed = state.foundation && state.fill;
    m_impl->gradientSelfTestPassed = state.foundation && state.gradient;
    m_impl->vectorFeatherSelfTestPassed =
        state.foundation && state.vectorFeather;
    m_impl->displayTransformSelfTestPassed =
        state.foundation && state.displayTransform;
    m_impl->managedAdjustmentTransformSelfTestPassed =
        state.foundation && state.managedAdjustmentTransforms;
    m_impl->approvedAdjustmentMask = state.foundation
        ? state.approvedAdjustmentMask : 0;
}

bool WebGpuContext::fillGpuApproved() const
{
    return m_impl->fillSelfTestPassed;
}

bool WebGpuContext::gradientGpuApproved() const
{
    return m_impl->gradientSelfTestPassed;
}

bool WebGpuContext::vectorFeatherGpuApproved() const
{
    return m_impl->vectorFeatherSelfTestPassed;
}

bool WebGpuContext::displayTransformGpuApproved() const
{
    return m_impl->displayTransformSelfTestPassed;
}

bool WebGpuContext::managedAdjustmentTransformsGpuApproved() const
{
    return m_impl->managedAdjustmentTransformSelfTestPassed;
}

bool WebGpuContext::adjustmentGpuApproved(const AdjustmentType type) const
{
    const quint32 bit = quint32(1) << static_cast<quint32>(type);
    return (m_impl->approvedAdjustmentMask & bit) != 0;
}

quint32 WebGpuContext::approvedAdjustmentMask() const
{
    return m_impl->approvedAdjustmentMask;
}

QString WebGpuContext::statusText() const
{
    return m_impl->status;
}

} // namespace vfx
