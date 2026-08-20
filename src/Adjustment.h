#pragma once

#include "VectorLayer.h"
#include "TextLayer.h"
#include "SmartLayerFoundation.h"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QStringList>
#include <QTransform>
#include <QString>
#include <QUuid>
#include <QVector>

#include <array>
#include <variant>

namespace vfx {

struct LayerNode;

enum class AdjustmentType {
    Exposure,
    Contrast,
    Saturation,
    Levels,
    Curves,
    HueSaturation,
    Vibrance,
    WhiteBalance,
    ColourBalance,
    ChannelMixer,
    BlackAndWhite,
    GradientMap,
    Posterise,
    Threshold,
    Lut,
    ShadowsHighlights,
    GaussianBlur,
    BoxBlur,
    UnsharpMask,
    HighPass,
    // Append-only milestone identifiers. Existing numeric values are project
    // and preset compatibility contracts and must never be reordered.
    Invert,
    PhotoFilter,
    SelectiveColour,
    Vignette,
    RgbSplit,
    ChromaticAberrationCorrection,
    SurfaceBlur,
    MotionBlur,
    RadialBlur
};

enum class AdjustmentProcessingDomain : quint8 {
    EncodedWorking,
    LinearWorking,
    EncodedSrgb,
    RawComponents,
    LutContract
};

enum class AdjustmentChannel {
    Rgb = 0,
    Red = 1,
    Green = 2,
    Blue = 3
};

enum class CurveInterpolation {
    Smooth,
    Linear
};

enum class HueSaturationRange {
    Reds = 0,
    Yellows = 1,
    Greens = 2,
    Cyans = 3,
    Blues = 4,
    Magentas = 5
};

enum class ColourBalanceRange {
    Shadows = 0,
    Midtones = 1,
    Highlights = 2
};

enum class SelectiveColourRange {
    Reds = 0,
    Yellows = 1,
    Greens = 2,
    Cyans = 3,
    Blues = 4,
    Magentas = 5,
    Whites = 6,
    Neutrals = 7,
    Blacks = 8
};

enum class SelectiveColourMethod {
    Relative = 0,
    Absolute = 1
};

enum class RadialBlurMode {
    Spin = 0,
    Zoom = 1
};

enum class ChannelMixerOutput {
    Red = 0,
    Green = 1,
    Blue = 2
};

enum class GradientInterpolation {
    Linear,
    Smooth,
    Constant
};

enum class ThresholdSource {
    Luminance,
    Red,
    Green,
    Blue
};

enum class LutDomainSource {
    DefaultRange,
    DomainDirective,
    InputRangeDirective,
    LegacyPersisted
};

enum class LutInterpolation {
    Trilinear,
    Tetrahedral
};

enum class LutProcessingMode {
    EncodedDocument,
    LinearSrgb,
    RawComponents
};

// Optional named pipelines for LUTs that are only one component of a
// documented display transform. Generic leaves the .cube table under the
// explicit processing contract above. Tony McMapface and AgX Base sRGB
// reproduce their required scene-linear preprocessing and output encoding.
enum class LutOperatorProfile {
    Generic,
    TonyMcMapface,
    AgXBaseSrgb
};


struct ExposureParameters {
    double exposure = 0.0;
    double offset = 0.0;
    double gamma = 1.0;
    bool operator==(const ExposureParameters &) const = default;
};

struct ContrastParameters {
    double contrast = 0.0;
    double pivot = 0.5;
    bool operator==(const ContrastParameters &) const = default;
};

struct SaturationParameters {
    double saturation = 0.0;
    bool operator==(const SaturationParameters &) const = default;
};

struct LevelsChannelParameters {
    double inputBlack = 0.0;
    double inputWhite = 1.0;
    double gamma = 1.0;
    double outputBlack = 0.0;
    double outputWhite = 1.0;

    void normalise();
    bool operator==(const LevelsChannelParameters &) const = default;
};

struct LevelsParameters {
    std::array<LevelsChannelParameters, 4> channels;
    bool logarithmicHistogram = false;
    double autoClipShadows = 0.001;
    double autoClipHighlights = 0.001;

    LevelsChannelParameters &channel(AdjustmentChannel channel);
    const LevelsChannelParameters &channel(AdjustmentChannel channel) const;
    void normalise();
    bool operator==(const LevelsParameters &) const = default;
};

struct CurvePoint {
    double input = 0.0;
    double output = 0.0;
    bool operator==(const CurvePoint &) const = default;
};

struct CurveChannelParameters {
    QVector<CurvePoint> points {{0.0, 0.0}, {1.0, 1.0}};

    void normalise();
    bool operator==(const CurveChannelParameters &) const = default;
};

struct CurvesParameters {
    std::array<CurveChannelParameters, 4> channels;
    CurveInterpolation interpolation = CurveInterpolation::Smooth;
    bool logarithmicHistogram = false;

    CurveChannelParameters &channel(AdjustmentChannel channel);
    const CurveChannelParameters &channel(AdjustmentChannel channel) const;
    void normalise();
    bool operator==(const CurvesParameters &) const = default;
};
struct HueSaturationRangeParameters {
    double hue = 0.0;
    double saturation = 0.0;
    double lightness = 0.0;
    double centre = 0.0;
    double width = 60.0;
    double feather = 30.0;

    void normalise();
    bool operator==(const HueSaturationRangeParameters &) const = default;
};

struct HueSaturationParameters {
    double hue = 0.0;
    double saturation = 0.0;
    double lightness = 0.0;
    std::array<HueSaturationRangeParameters, 6> ranges;

    HueSaturationParameters();
    HueSaturationRangeParameters &range(HueSaturationRange range);
    const HueSaturationRangeParameters &range(HueSaturationRange range) const;
    void normalise();
    bool operator==(const HueSaturationParameters &) const = default;
};

struct VibranceParameters {
    double vibrance = 0.0;
    double saturation = 0.0;
    double skinProtection = 65.0;

    void normalise();
    bool operator==(const VibranceParameters &) const = default;
};

struct WhiteBalanceParameters {
    double temperature = 0.0;
    double tint = 0.0;

    void normalise();
    bool operator==(const WhiteBalanceParameters &) const = default;
};

struct ColourBalanceRangeParameters {
    double cyanRed = 0.0;
    double magentaGreen = 0.0;
    double yellowBlue = 0.0;

    void normalise();
    bool operator==(const ColourBalanceRangeParameters &) const = default;
};

struct ColourBalanceParameters {
    std::array<ColourBalanceRangeParameters, 3> ranges;
    bool preserveLuminosity = true;

    ColourBalanceRangeParameters &range(ColourBalanceRange range);
    const ColourBalanceRangeParameters &range(ColourBalanceRange range) const;
    void normalise();
    bool operator==(const ColourBalanceParameters &) const = default;
};

struct ChannelMixerChannelParameters {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double constant = 0.0;

    void normalise();
    bool operator==(const ChannelMixerChannelParameters &) const = default;
};

struct ChannelMixerParameters {
    std::array<ChannelMixerChannelParameters, 3> outputs;
    ChannelMixerChannelParameters monochrome {40.0, 40.0, 20.0, 0.0};
    bool monochromeEnabled = false;

    ChannelMixerParameters();
    ChannelMixerChannelParameters &output(ChannelMixerOutput output);
    const ChannelMixerChannelParameters &output(ChannelMixerOutput output) const;
    void normalise();
    bool operator==(const ChannelMixerParameters &) const = default;
};

struct BlackAndWhiteParameters {
    // Reds, Yellows, Greens, Cyans, Blues and Magentas. A value of 100
    // preserves the source luminance for that colour family.
    std::array<double, 6> colourWeights {100.0, 100.0, 100.0, 100.0, 100.0, 100.0};
    bool tintEnabled = false;
    double tintHue = 30.0;
    double tintSaturation = 20.0;

    void normalise();
    bool operator==(const BlackAndWhiteParameters &) const = default;
};

struct GradientStop {
    double position = 0.0;
    QColor colour = Qt::black;

    bool operator==(const GradientStop &) const = default;
};

struct GradientMapParameters {
    QVector<GradientStop> stops {{0.0, Qt::black}, {1.0, Qt::white}};
    bool reverse = false;
    GradientInterpolation interpolation = GradientInterpolation::Linear;

    void normalise();
    bool operator==(const GradientMapParameters &) const = default;
};

struct PosteriseParameters {
    int levels = 4;

    void normalise();
    bool operator==(const PosteriseParameters &) const = default;
};

struct ThresholdParameters {
    double threshold = 0.5;
    ThresholdSource source = ThresholdSource::Luminance;

    void normalise();
    bool operator==(const ThresholdParameters &) const = default;
};

// Invert intentionally has no user parameters, matching the ordinary
// one-click adjustment-layer workflow while still occupying a typed, stable
// serialization slot. It inverts straight RGB code values and preserves Alpha.
struct InvertParameters {
    void normalise() {}
    bool operator==(const InvertParameters &) const = default;
};

// Photo Filter stores only its actual rendering contract. Named warming and
// cooling choices are Inspector/preset conveniences that resolve to this
// embedded colour, so projects remain independent of future UI label changes.
struct PhotoFilterParameters {
    QColor colour = QColor(236, 138, 0);
    double density = 25.0;
    bool preserveLuminosity = true;

    void normalise();
    bool operator==(const PhotoFilterParameters &) const = default;
};

struct SelectiveColourRangeParameters {
    double cyan = 0.0;
    double magenta = 0.0;
    double yellow = 0.0;
    double black = 0.0;

    void normalise();
    bool operator==(const SelectiveColourRangeParameters &) const = default;
};

struct SelectiveColourParameters {
    std::array<SelectiveColourRangeParameters, 9> ranges;
    SelectiveColourMethod method = SelectiveColourMethod::Relative;

    SelectiveColourRangeParameters &range(SelectiveColourRange selected);
    const SelectiveColourRangeParameters &range(SelectiveColourRange selected) const;
    void normalise();
    bool operator==(const SelectiveColourParameters &) const = default;
};


struct VignetteParameters {
    // Positive Amount darkens; negative Amount brightens. Invert swaps the
    // edge mask to the centre, allowing bright centre glow or dark centre
    // treatments without changing the persisted meaning of Amount.
    double amount = 25.0;
    // Uniform geometry scale relative to the document-sized ellipse. Values
    // above 100 allow the vignette boundary to extend beyond the canvas so
    // only corners need be affected. Existing projects default to 100.
    double size = 100.0;
    double midpoint = 50.0;
    double roundness = 0.0;
    double feather = 50.0;
    double centreX = 0.0;
    double centreY = 0.0;
    double rotation = 0.0;
    double highlightProtection = 0.0;
    bool inverted = false;
    bool tintEnabled = false;
    QColor tint = Qt::black;

    void normalise();
    bool operator==(const VignetteParameters &) const = default;
};

struct RgbSplitParameters {
    // Green remains anchored. Red and Blue are sampled independently in
    // document pixels with clamped edges and bilinear filtering.
    double redOffsetX = -6.0;
    double redOffsetY = 0.0;
    double blueOffsetX = 6.0;
    double blueOffsetY = 0.0;

    void normalise();
    bool operator==(const RgbSplitParameters &) const = default;
};

struct ChromaticAberrationCorrectionParameters {
    // Signed channel displacement at the furthest document edge. Positive
    // values move channel detail outwards; negative values move it inwards.
    double redEdgeShift = 0.0;
    double blueEdgeShift = 0.0;
    double centreX = 0.0;
    double centreY = 0.0;
    double falloff = 1.0;

    void normalise();
    bool operator==(const ChromaticAberrationCorrectionParameters &) const = default;
};

struct SurfaceBlurParameters {
    // Radius defines the local low-pass support. Threshold is stored in stable
    // 8-bit code-value units and controls how strongly edges resist smoothing.
    double radius = 12.0;
    double threshold = 20.0;

    void normalise();
    bool operator==(const SurfaceBlurParameters &) const = default;
};

struct MotionBlurParameters {
    // Distance is the complete centred motion trail in document pixels.
    double distance = 24.0;
    double angle = 0.0;
    int samples = 24;
    bool affectAlpha = true;

    void normalise();
    bool operator==(const MotionBlurParameters &) const = default;
};

struct RadialBlurParameters {
    // Amount is the maximum local displacement, in document pixels, at the
    // furthest document edge. This bounded contract keeps tiled dependencies
    // exact for both Spin and Zoom modes.
    RadialBlurMode mode = RadialBlurMode::Spin;
    double amount = 20.0;
    double centreX = 0.0;
    double centreY = 0.0;
    int samples = 24;
    bool affectAlpha = true;

    void normalise();
    bool operator==(const RadialBlurParameters &) const = default;
};

struct ShadowsHighlightsParameters {
    double shadowAmount = 0.0;
    double shadowTonalWidth = 50.0;
    double highlightAmount = 0.0;
    double highlightTonalWidth = 50.0;
    double radius = 80.0;
    double midtoneContrast = 0.0;
    double colourCorrection = 20.0;

    void normalise();
    bool operator==(const ShadowsHighlightsParameters &) const = default;
};

// 0.13.0b spatial filters intentionally persist only stable, document-space
// parameters. Edge handling is Clamp for this first public stage so projects
// cannot request an unsupported tiled dependency topology. Alpha behaviour is
// explicit for blur filters; sharpen/high-pass always preserve source alpha.
struct GaussianBlurParameters {
    double radius = 8.0;
    bool affectAlpha = true;

    void normalise();
    bool operator==(const GaussianBlurParameters &) const = default;
};

struct BoxBlurParameters {
    double radius = 4.0;
    bool affectAlpha = true;

    void normalise();
    bool operator==(const BoxBlurParameters &) const = default;
};

struct UnsharpMaskParameters {
    double radius = 2.0;
    double amount = 100.0;
    // Stored in 8-bit code-value units and scaled consistently for 16-bit.
    double threshold = 0.0;

    void normalise();
    bool operator==(const UnsharpMaskParameters &) const = default;
};

struct HighPassParameters {
    double radius = 10.0;
    bool monochrome = false;

    void normalise();
    bool operator==(const HighPassParameters &) const = default;
};

struct LutParameters {
    QString title;
    QString sourceName;
    int shaperSize = 0;
    int cubeSize = 0;
    QVector<float> shaperData;
    QVector<float> cubeData;
    std::array<double, 3> shaperDomainMin {0.0, 0.0, 0.0};
    std::array<double, 3> shaperDomainMax {1.0, 1.0, 1.0};
    std::array<double, 3> cubeDomainMin {0.0, 0.0, 0.0};
    std::array<double, 3> cubeDomainMax {1.0, 1.0, 1.0};
    LutDomainSource shaperDomainSource = LutDomainSource::DefaultRange;
    LutDomainSource cubeDomainSource = LutDomainSource::DefaultRange;
    // New imports use the higher-quality tetrahedral CPU evaluator. Projects
    // saved by adjustment schemas 1-7 migrate explicitly to trilinear so their
    // historical appearance remains unchanged until the user chooses otherwise.
    LutInterpolation interpolation = LutInterpolation::Tetrahedral;
    // Defines the colour-domain contract surrounding the table. EncodedDocument
    // applies the table in encoded sRGB values, LinearSrgb applies it in linear
    // sRGB / Rec.709 primaries, and RawComponents performs no transfer-function
    // conversion. Strength always blends in the document's stored component
    // space after the table output is returned to that space. Named operator
    // profiles replace this generic surrounding contract with their fixed
    // documented pipeline while retaining the same final Strength space.
    LutProcessingMode processingMode = LutProcessingMode::EncodedDocument;
    LutOperatorProfile operatorProfile = LutOperatorProfile::Generic;
    double strength = 100.0;
    // Deterministic content fingerprint calculated once after parsing or loading.
    // Renderer cache keys use this instead of rescanning a large LUT per tile.
    quint64 tableFingerprint = 0;
    // Derived GPU transport capabilities. gpuDisplayRangeCompatible remains a
    // baseline/conformance fact for projects created before 0.10.1f; it no
    // longer gates native evaluation. The floating-point path accepts extended
    // values whenever the table and domains are representable by RGBA16Float
    // texture data plus f32 uniforms.
    bool gpuDisplayRangeCompatible = true;
    // Cached table-only capability. Domain compatibility is recomputed on
    // every normalise() call so temporarily entering an unsupported domain
    // does not permanently strand an otherwise valid LUT on the CPU.
    bool gpuTableHalfFloatCompatible = true;
    bool gpuHalfFloatCompatible = true;

    bool hasShaper() const;
    bool hasCube() const;
    bool hasData() const;
    void clear();
    void normalise();
    bool operator==(const LutParameters &) const = default;
};

using AdjustmentParameters = std::variant<ExposureParameters,
                                          ContrastParameters,
                                          SaturationParameters,
                                          LevelsParameters,
                                          CurvesParameters,
                                          HueSaturationParameters,
                                          VibranceParameters,
                                          WhiteBalanceParameters,
                                          ColourBalanceParameters,
                                          ChannelMixerParameters,
                                          BlackAndWhiteParameters,
                                          GradientMapParameters,
                                          PosteriseParameters,
                                          ThresholdParameters,
                                          LutParameters,
                                          ShadowsHighlightsParameters,
                                          GaussianBlurParameters,
                                          BoxBlurParameters,
                                          UnsharpMaskParameters,
                                          HighPassParameters,
                                          InvertParameters,
                                          PhotoFilterParameters,
                                          SelectiveColourParameters,
                                          VignetteParameters,
                                          RgbSplitParameters,
                                          ChromaticAberrationCorrectionParameters,
                                          SurfaceBlurParameters,
                                          MotionBlurParameters,
                                          RadialBlurParameters>;

struct AdjustmentData {
    static constexpr quint32 CurrentSchema = 16;

    quint32 schema = CurrentSchema;
    AdjustmentType type = AdjustmentType::Exposure;
    AdjustmentParameters parameters = ExposureParameters {};

    void reset(AdjustmentType newType);
    void normalise();
    QJsonObject toJson(bool *ok = nullptr) const;
    static AdjustmentData fromJson(const QJsonObject &object,
                                   AdjustmentType fallbackType,
                                   bool *ok = nullptr);
    bool operator==(const AdjustmentData &) const = default;
};

// 0.14.0f persistent, ordered per-Smart-Layer Live Filter entry. Live Filters
// intentionally reuse the proven typed AdjustmentData operator payload while
// remaining semantically distinct from Adjustment Layers: they process only
// their owning Smart Layer between the instance transform and layer mask.
struct LiveFilter {
    // Schema 2 adds an optional independent per-filter raster mask. The mask is
    // stored in the Smart Layer's local reference space and follows the Smart
    // instance transform just like an ordinary layer mask, but is applied
    // immediately after this filter stage rather than after the whole layer.
    static constexpr quint32 CurrentSchema = 2;

    quint32 schema = CurrentSchema;
    QUuid id = QUuid::createUuid();
    bool enabled = true;
    quint64 revision = 1;
    AdjustmentData adjustment;
    QImage maskImage;
    QSize maskReferenceSize;
    QPointF maskReferenceOrigin;
    bool maskEnabled = true;
    bool maskInverted = false;

    bool hasMask() const { return !maskImage.isNull(); }
    void normalise();
    bool isSafe() const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static LiveFilter fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const LiveFilter &) const = default;
};

bool adjustmentTypeSupportsLiveFilter(AdjustmentType type);
QByteArray adjustmentRenderIdentity(const AdjustmentData &data);
QSize liveFilterStackSpatialRadius2D(const QVector<LiveFilter> &filters);
int liveFilterStackSpatialRadius(const QVector<LiveFilter> &filters);
qint64 liveFilterStackEstimatedBytes(const QVector<LiveFilter> &filters);

enum class LayerType {
    BaseImage,
    Raster,
    Adjustment,
    Group,
    Vector,
    Text,
    Smart
};

enum class GroupCompositeMode {
    Isolated,
    PassThrough
};

enum class BlendMode {
    Copy,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColourDodge,
    ColourBurn,
    Add,
    Subtract,
    Difference,
    Exclusion
};

// Persistent per-layer Layer Effect identity. Layer Effects are deliberately
// separate from Live Filters: they derive appearance from the owning layer's
// rendered content/coverage and composite that generated appearance around the
// source layer. Schema 2 added the shared shadow/glow payload; schema 3 added
// Stroke/Colour Overlay/Gradient Overlay parameters; schema 4 adds the
// authored Bevel & Emboss lighting payload for 0.14.0k.
enum class LayerEffectType {
    DropShadow,
    InnerShadow,
    OuterGlow,
    InnerGlow,
    Stroke,
    ColourOverlay,
    GradientOverlay,
    BevelEmboss
};

enum class LayerEffectStrokePosition {
    Inside,
    Centre,
    Outside
};

enum class LayerEffectGradientStyle {
    Linear,
    Radial
};

enum class LayerEffectBevelStyle {
    InnerBevel,
    OuterBevel,
    Emboss,
    PillowEmboss
};

enum class LayerEffectBevelDirection {
    Up,
    Down
};

struct LayerEffect {
    static constexpr quint32 CurrentSchema = 4;

    quint32 schema = CurrentSchema;
    QUuid id = QUuid::createUuid();
    LayerEffectType type = LayerEffectType::DropShadow;
    bool enabled = false;
    quint64 revision = 1;

    // Shared effect payload. Colour is authored in the owning document working-
    // space component convention. Opacity is independent from colour alpha.
    // Spread is percentage (0-100); size and distance are document-space pixels.
    QColor colour = QColor(0, 0, 0);
    double effectOpacity = 0.75;
    BlendMode effectBlendMode = BlendMode::Multiply;
    double angleDegrees = 135.0;
    double distance = 10.0;
    double spread = 0.0;
    double size = 10.0;

    // 0.14.0j Stroke and Gradient Overlay payload. Gradient Overlay reuses the
    // same stop/interpolation model as Gradient Map so the established editor,
    // persistence validation and deterministic interpolation semantics remain
    // shared without making the effect an Adjustment Layer.
    LayerEffectStrokePosition strokePosition = LayerEffectStrokePosition::Outside;
    QVector<GradientStop> gradientStops {{0.0, Qt::black}, {1.0, Qt::white}};
    GradientInterpolation gradientInterpolation = GradientInterpolation::Linear;
    LayerEffectGradientStyle gradientStyle = LayerEffectGradientStyle::Linear;
    double gradientAngleDegrees = 90.0;
    double gradientScale = 100.0;
    bool gradientReverse = false;

    // 0.14.0k Bevel & Emboss payload. The coverage-derived signed-distance
    // field remains renderer-owned; these are only persistent authored controls.
    LayerEffectBevelStyle bevelStyle = LayerEffectBevelStyle::InnerBevel;
    LayerEffectBevelDirection bevelDirection = LayerEffectBevelDirection::Up;
    double bevelDepth = 100.0;
    double bevelSoften = 0.0;
    double bevelAltitudeDegrees = 30.0;
    QColor bevelHighlightColour = QColor(255, 255, 255);
    BlendMode bevelHighlightBlendMode = BlendMode::Screen;
    double bevelHighlightOpacity = 0.75;
    QColor bevelShadowColour = QColor(0, 0, 0);
    BlendMode bevelShadowBlendMode = BlendMode::Multiply;
    double bevelShadowOpacity = 0.75;

    void normalise();
    bool isSafe() const;
    QJsonObject toJson(bool *ok = nullptr) const;
    static LayerEffect fromJson(const QJsonObject &object, bool *ok = nullptr);
    bool operator==(const LayerEffect &) const = default;
};

QString layerEffectTypeToString(LayerEffectType type);
QString layerEffectTypeDisplayName(LayerEffectType type);
LayerEffectType layerEffectTypeFromString(const QString &value, bool *ok = nullptr);
bool layerEffectTypeHasRenderer(LayerEffectType type);
QString layerEffectImplementationRevision(LayerEffectType type);
bool layerTypeSupportsLayerEffects(LayerType type);
qint64 layerEffectStackEstimatedBytes(const QVector<LayerEffect> &effects);
// Stable render-semantic hook used by the tiled compositor/cache once concrete
// effect renderers land. Disabled definition-only entries do not affect pixels.
QByteArray layerEffectStackRenderIdentity(const QVector<LayerEffect> &effects);
QSize layerEffectStackSpatialRadius2D(const QVector<LayerEffect> &effects);

QString adjustmentTypeToString(AdjustmentType type);
AdjustmentProcessingDomain adjustmentProcessingDomain(const AdjustmentData &data);
QString adjustmentProcessingDomainName(AdjustmentProcessingDomain domain);
bool adjustmentRequiresManagedDomainTransform(const AdjustmentData &data);
AdjustmentType adjustmentTypeFromString(const QString &value, bool *ok = nullptr);
QString defaultAdjustmentName(AdjustmentType type);
QString curveInterpolationToString(CurveInterpolation interpolation);
CurveInterpolation curveInterpolationFromString(const QString &value, bool *ok = nullptr);
QString hueSaturationRangeDisplayName(HueSaturationRange range);
QString colourBalanceRangeDisplayName(ColourBalanceRange range);
QString channelMixerOutputDisplayName(ChannelMixerOutput output);
QString gradientInterpolationToString(GradientInterpolation interpolation);
GradientInterpolation gradientInterpolationFromString(const QString &value, bool *ok = nullptr);
QString thresholdSourceToString(ThresholdSource source);
QString thresholdSourceDisplayName(ThresholdSource source);
ThresholdSource thresholdSourceFromString(const QString &value, bool *ok = nullptr);
QString lutDomainSourceToString(LutDomainSource source);
QString lutDomainSourceDisplayName(LutDomainSource source);
LutDomainSource lutDomainSourceFromString(const QString &value, bool *ok = nullptr);
QString lutInterpolationToString(LutInterpolation interpolation);
QString lutInterpolationDisplayName(LutInterpolation interpolation);
LutInterpolation lutInterpolationFromString(const QString &value, bool *ok = nullptr);
QString lutProcessingModeToString(LutProcessingMode mode);
QString lutProcessingModeDisplayName(LutProcessingMode mode);
LutProcessingMode lutProcessingModeFromString(const QString &value, bool *ok = nullptr);
QString lutOperatorProfileToString(LutOperatorProfile profile);
QString lutOperatorProfileDisplayName(LutOperatorProfile profile);
LutOperatorProfile lutOperatorProfileFromString(const QString &value, bool *ok = nullptr);

QString layerTypeToString(LayerType type);
LayerType layerTypeFromString(const QString &value, bool *ok = nullptr);
QString defaultLayerName(LayerType type, AdjustmentType adjustmentType = AdjustmentType::Exposure);

QString blendModeToString(BlendMode mode);
QString blendModeDisplayName(BlendMode mode);
BlendMode blendModeFromString(const QString &value, bool *ok = nullptr);
QVector<BlendMode> availableBlendModes();

bool adjustmentIsSpatial(AdjustmentType type);
QSize adjustmentSpatialRadius2D(const AdjustmentData &data);
int adjustmentSpatialRadius(const AdjustmentData &data);

QString groupCompositeModeToString(GroupCompositeMode mode);
QString groupCompositeModeDisplayName(GroupCompositeMode mode);
GroupCompositeMode groupCompositeModeFromString(const QString &value, bool *ok = nullptr);

struct LayerNode {
    static constexpr int MaximumTreeDepth = 128;
    static constexpr int MaximumTreeLayerCount = 8192;
    static constexpr int MaximumLiveFilterCount = 64;
    static constexpr int MaximumLayerEffectCount = 32;

    QUuid id = QUuid::createUuid();
    LayerType type = LayerType::Raster;
    QString name = defaultLayerName(type);
    bool visible = true;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Copy;
    GroupCompositeMode groupCompositeMode = GroupCompositeMode::Isolated;
    QTransform transform;
    quint64 revision = 1;

    // Schema-versioned typed parameters are authoritative. These scalar mirrors
    // remain for public version-6 compatibility and legacy tests.
    AdjustmentType adjustmentType = AdjustmentType::Exposure;
    AdjustmentData adjustment;
    double exposure = 0.0;
    double contrast = 0.0;
    double saturation = 0.0;
    double blackPoint = 0.0;
    double whitePoint = 1.0;
    double gamma = 1.0;

    AdjustmentData effectiveAdjustmentData() const;
    LevelsParameters effectiveLevelsParameters() const;
    CurvesParameters effectiveCurvesParameters() const;
    void setAdjustmentData(const AdjustmentData &data);
    void resetAdjustmentParameters(AdjustmentType type);
    void setExposure(double value);
    void setExposureParameters(const ExposureParameters &parameters);
    void setContrast(double value);
    void setContrastParameters(const ContrastParameters &parameters);
    void setSaturation(double value);
    void setSaturationParameters(const SaturationParameters &parameters);
    void setLevelsParameters(const LevelsParameters &parameters);
    void setCurvesParameters(const CurvesParameters &parameters);
    void setHueSaturationParameters(const HueSaturationParameters &parameters);
    void setVibranceParameters(const VibranceParameters &parameters);
    void setWhiteBalanceParameters(const WhiteBalanceParameters &parameters);
    void setColourBalanceParameters(const ColourBalanceParameters &parameters);
    void setChannelMixerParameters(const ChannelMixerParameters &parameters);
    void setBlackAndWhiteParameters(const BlackAndWhiteParameters &parameters);
    void setGradientMapParameters(const GradientMapParameters &parameters);
    void setPosteriseParameters(const PosteriseParameters &parameters);
    void setThresholdParameters(const ThresholdParameters &parameters);
    void setInvertParameters(const InvertParameters &parameters = {});
    void setPhotoFilterParameters(const PhotoFilterParameters &parameters);
    void setSelectiveColourParameters(const SelectiveColourParameters &parameters);
    void setVignetteParameters(const VignetteParameters &parameters);
    void setRgbSplitParameters(const RgbSplitParameters &parameters);
    void setChromaticAberrationCorrectionParameters(
        const ChromaticAberrationCorrectionParameters &parameters);
    void setSurfaceBlurParameters(const SurfaceBlurParameters &parameters);
    void setMotionBlurParameters(const MotionBlurParameters &parameters);
    void setRadialBlurParameters(const RadialBlurParameters &parameters);
    void setLutParameters(const LutParameters &parameters);
    void setShadowsHighlightsParameters(const ShadowsHighlightsParameters &parameters);
    void setGaussianBlurParameters(const GaussianBlurParameters &parameters);
    void setBoxBlurParameters(const BoxBlurParameters &parameters);
    void setUnsharpMaskParameters(const UnsharpMaskParameters &parameters);
    void setHighPassParameters(const HighPassParameters &parameters);

    VectorLayerData vectorData;
    TextLayerData textData;
    SmartLayerReference smartSource;
    SmartTransformState smartTransform;
    // Ordered per-instance Live Filter stack. Authoritative Smart Source
    // contents do not own these entries, so two instances of one source may
    // retain independent non-destructive filter stacks.
    QVector<LiveFilter> liveFilters;
    // Ordered per-layer Layer Effect stack. Unlike Live Filters this is valid
    // on ordinary pixel/vector/text layers as well as Smart Layers and remains
    // outside the authoritative Smart Source identity.
    QVector<LayerEffect> layerEffects;

    // Runtime-only exact presentation resolved from the document-owned Smart
    // Source registry. Authoritative embedded contents never live here and
    // these fields are deliberately omitted from LayerNode JSON/session layer
    // payloads; PhotoDocument re-synchronises them from SmartSourceDescriptor
    // after load, Undo/Redo and source revision changes.
    QImage smartPresentationImage;
    QSize smartPresentationReferenceSize;
    QPointF smartPresentationReferenceOrigin;

    QImage rasterImage;
    QSize rasterReferenceSize;
    QPointF rasterReferenceOrigin;
    QImage maskImage;
    QSize maskReferenceSize;
    QPointF maskReferenceOrigin;
    bool maskEnabled = true;
    bool maskInverted = false;
    QVector<LayerNode> children;

    bool hasMask() const { return !maskImage.isNull(); }
    bool isContainer() const { return type == LayerType::Group; }
    bool isAdjustment() const { return type == LayerType::Adjustment; }
    bool isVector() const { return type == LayerType::Vector; }
    bool isText() const { return type == LayerType::Text; }
    bool isSmart() const { return type == LayerType::Smart; }
    bool operator==(const LayerNode &other) const;

    QJsonObject toJson(bool *ok = nullptr) const;
    static LayerNode fromJson(const QJsonObject &object,
                              bool *ok = nullptr,
                              QStringList *warnings = nullptr);
};

bool layerTreeRequiresManagedDomainTransform(const QVector<LayerNode> &layers);
QSize maximumSpatialAdjustmentRadius2D(const QVector<LayerNode> &layers);
int maximumSpatialAdjustmentRadius(const QVector<LayerNode> &layers);

} // namespace vfx
