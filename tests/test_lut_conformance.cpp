#include "AdjustmentPresetStore.h"
#include "CubeLut.h"
#include "ImageProcessor.h"
#include "PhotoDocument.h"
#include "DocumentSession.h"
#include "SessionCache.h"

#include <QColor>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QRgba64>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace vfx;

namespace {

constexpr double ScalarTolerance = 2.0e-6;

double encodedToLinearExtended(const double value)
{
    return value <= 0.04045
        ? value / 12.92
        : std::pow((value + 0.055) / 1.055, 2.4);
}

double linearToEncodedExtended(const double value)
{
    return value <= 0.0031308
        ? value * 12.92
        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

std::array<double, 3> referenceTonyIdentity(
    const std::array<double, 3> &documentInput,
    const LutDocumentTransfer transfer)
{
    std::array<double, 3> output {};
    for (int channel = 0; channel < 3; ++channel) {
        const double stored = documentInput[static_cast<std::size_t>(channel)];
        const double linear = transfer == LutDocumentTransfer::EncodedSrgb
            ? encodedToLinearExtended(stored) : stored;
        const double mapped = linear / (linear + 1.0);
        output[static_cast<std::size_t>(channel)] =
            transfer == LutDocumentTransfer::EncodedSrgb
                ? linearToEncodedExtended(mapped) : mapped;
    }
    return output;
}

std::array<double, 3> referenceAgXIdentity(
    const std::array<double, 3> &documentInput,
    const LutDocumentTransfer transfer)
{
    constexpr double minimumExposure = -12.47393;
    constexpr double maximumExposure = 12.5260688117;
    constexpr double span = maximumExposure - minimumExposure;
    std::array<double, 3> linear {};
    for (int channel = 0; channel < 3; ++channel) {
        const double stored = documentInput[static_cast<std::size_t>(channel)];
        linear[static_cast<std::size_t>(channel)] =
            transfer == LutDocumentTransfer::EncodedSrgb
                ? encodedToLinearExtended(stored) : stored;
    }
    const std::array<double, 3> filmLight {
        linear[0] * 0.5594630473276861
            + linear[1] * 0.3047758110283366
            + linear[2] * 0.1358129414038276,
        linear[0] * 0.0762332608733703
            + linear[1] * 0.7879523952184488
            + linear[2] * 0.1357748488287584,
        linear[0] * 0.0655375095152927
            + linear[1] * 0.1645427298716744
            + linear[2] * 0.7697415276874705
    };
    std::array<double, 3> output {};
    for (int channel = 0; channel < 3; ++channel) {
        const double value = filmLight[static_cast<std::size_t>(channel)];
        const double allocated = value > 0.0
            ? std::clamp((std::log2(value) - minimumExposure) / span, 0.0, 1.0)
            : 0.0;
        const double mapped = std::pow(std::max(0.0, allocated), 2.4);
        output[static_cast<std::size_t>(channel)] =
            transfer == LutDocumentTransfer::EncodedSrgb
                ? linearToEncodedExtended(mapped) : mapped;
    }
    return output;
}

QString fixtureDirectory()
{
    return QDir(QString::fromUtf8(VFXPHOTOLAB_TEST_SOURCE_DIR))
        .filePath(QStringLiteral("tests/fixtures/lut"));
}

QString fixturePath(const QString &name)
{
    return QDir(fixtureDirectory()).filePath(name);
}

bool parseFinite(const QString &text, double *value)
{
    bool ok = false;
    const double decoded = text.toDouble(&ok);
    if (!ok || !std::isfinite(decoded)) {
        return false;
    }
    if (value) {
        *value = decoded;
    }
    return true;
}

QStringList tokensForLine(QString line)
{
    const int comment = line.indexOf(QLatin1Char('#'));
    if (comment >= 0) {
        line.truncate(comment);
    }
    return line.trimmed().split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

struct ReferenceLut {
    int shaperSize = 0;
    int cubeSize = 0;
    std::array<double, 3> shaperMin {0.0, 0.0, 0.0};
    std::array<double, 3> shaperMax {1.0, 1.0, 1.0};
    std::array<double, 3> cubeMin {0.0, 0.0, 0.0};
    std::array<double, 3> cubeMax {1.0, 1.0, 1.0};
    QVector<std::array<double, 3>> shaperRows;
    QVector<std::array<double, 3>> cubeRows;

    bool parseFile(const QString &path, QString *error)
    {
        if (error) {
            error->clear();
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (error) {
                *error = QStringLiteral("Could not open %1").arg(path);
            }
            return false;
        }

        QVector<std::array<double, 3>> rows;
        bool sawDomainMin = false;
        bool sawDomainMax = false;
        std::array<double, 3> genericMin {0.0, 0.0, 0.0};
        std::array<double, 3> genericMax {1.0, 1.0, 1.0};
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
            const QStringList tokens = tokensForLine(lines.at(lineIndex));
            if (tokens.isEmpty()) {
                continue;
            }
            const QString keyword = tokens.constFirst().toUpper();
            const auto fail = [&](const QString &message) {
                if (error) {
                    *error = QStringLiteral("%1 at line %2")
                                 .arg(message)
                                 .arg(lineIndex + 1);
                }
                return false;
            };
            if (keyword == QStringLiteral("TITLE")) {
                continue;
            }
            if (keyword == QStringLiteral("LUT_1D_SIZE")
                || keyword == QStringLiteral("LUT_3D_SIZE")) {
                if (tokens.size() != 2) {
                    return fail(QStringLiteral("Invalid size directive"));
                }
                bool ok = false;
                const int size = tokens.at(1).toInt(&ok);
                if (!ok || size < 2 || size > 65536) {
                    return fail(QStringLiteral("Invalid LUT size"));
                }
                if (keyword == QStringLiteral("LUT_1D_SIZE")) {
                    shaperSize = size;
                } else {
                    if (size > 65) {
                        return fail(QStringLiteral("3D fixture exceeds safety limit"));
                    }
                    cubeSize = size;
                }
                continue;
            }
            if (keyword == QStringLiteral("DOMAIN_MIN")
                || keyword == QStringLiteral("DOMAIN_MAX")) {
                if (tokens.size() != 4) {
                    return fail(QStringLiteral("Invalid domain directive"));
                }
                std::array<double, 3> values {};
                for (int channel = 0; channel < 3; ++channel) {
                    if (!parseFinite(tokens.at(channel + 1), &values[static_cast<std::size_t>(channel)])) {
                        return fail(QStringLiteral("Invalid domain value"));
                    }
                }
                if (keyword == QStringLiteral("DOMAIN_MIN")) {
                    genericMin = values;
                    sawDomainMin = true;
                } else {
                    genericMax = values;
                    sawDomainMax = true;
                }
                continue;
            }
            if (keyword == QStringLiteral("LUT_1D_INPUT_RANGE")
                || keyword == QStringLiteral("LUT_3D_INPUT_RANGE")) {
                if (tokens.size() != 3) {
                    return fail(QStringLiteral("Invalid input range directive"));
                }
                double minimum = 0.0;
                double maximum = 0.0;
                if (!parseFinite(tokens.at(1), &minimum)
                    || !parseFinite(tokens.at(2), &maximum)
                    || maximum <= minimum) {
                    return fail(QStringLiteral("Invalid input range"));
                }
                const std::array<double, 3> mins {minimum, minimum, minimum};
                const std::array<double, 3> maxs {maximum, maximum, maximum};
                if (keyword == QStringLiteral("LUT_1D_INPUT_RANGE")) {
                    shaperMin = mins;
                    shaperMax = maxs;
                } else {
                    cubeMin = mins;
                    cubeMax = maxs;
                }
                continue;
            }

            if (tokens.size() != 3) {
                return fail(QStringLiteral("Unexpected fixture content"));
            }
            std::array<double, 3> row {};
            for (int channel = 0; channel < 3; ++channel) {
                if (!parseFinite(tokens.at(channel), &row[static_cast<std::size_t>(channel)])) {
                    return fail(QStringLiteral("Invalid table value"));
                }
            }
            rows.push_back(row);
        }

        if (sawDomainMin) {
            if (shaperSize > 0) shaperMin = genericMin;
            if (cubeSize > 0) cubeMin = genericMin;
        }
        if (sawDomainMax) {
            if (shaperSize > 0) shaperMax = genericMax;
            if (cubeSize > 0) cubeMax = genericMax;
        }
        for (int channel = 0; channel < 3; ++channel) {
            if ((shaperSize > 0
                 && shaperMax[static_cast<std::size_t>(channel)]
                        <= shaperMin[static_cast<std::size_t>(channel)])
                || (cubeSize > 0
                    && cubeMax[static_cast<std::size_t>(channel)]
                           <= cubeMin[static_cast<std::size_t>(channel)])) {
                if (error) {
                    *error = QStringLiteral("Fixture contains a non-positive domain");
                }
                return false;
            }
        }

        const qsizetype shaperCount = shaperSize;
        const qsizetype cubeCount = cubeSize > 0
            ? qsizetype(cubeSize) * cubeSize * cubeSize : 0;
        if (rows.size() != shaperCount + cubeCount) {
            if (error) {
                *error = QStringLiteral("Fixture declares %1 rows but contains %2")
                             .arg(shaperCount + cubeCount)
                             .arg(rows.size());
            }
            return false;
        }
        shaperRows = rows.mid(0, shaperCount);
        cubeRows = rows.mid(shaperCount, cubeCount);
        return shaperSize > 0 || cubeSize > 0;
    }

    std::array<double, 3> evaluate(const std::array<double, 3> &input) const
    {
        std::array<double, 3> shaped = input;
        if (shaperSize > 0) {
            for (int channel = 0; channel < 3; ++channel) {
                const std::size_t c = static_cast<std::size_t>(channel);
                const double position = std::clamp(
                    (input[c] - shaperMin[c]) / (shaperMax[c] - shaperMin[c]),
                    0.0, 1.0) * (shaperSize - 1);
                const int left = std::clamp(static_cast<int>(std::floor(position)),
                                            0, shaperSize - 1);
                const int right = std::min(shaperSize - 1, left + 1);
                const double t = position - left;
                shaped[c] = shaperRows.at(left)[c]
                    + (shaperRows.at(right)[c] - shaperRows.at(left)[c]) * t;
            }
        }
        if (cubeSize <= 0) {
            return shaped;
        }

        std::array<int, 3> lower {};
        std::array<int, 3> upper {};
        std::array<double, 3> fraction {};
        for (int channel = 0; channel < 3; ++channel) {
            const std::size_t c = static_cast<std::size_t>(channel);
            const double position = std::clamp(
                (shaped[c] - cubeMin[c]) / (cubeMax[c] - cubeMin[c]),
                0.0, 1.0) * (cubeSize - 1);
            lower[c] = std::clamp(static_cast<int>(std::floor(position)),
                                  0, cubeSize - 1);
            upper[c] = std::min(cubeSize - 1, lower[c] + 1);
            fraction[c] = position - lower[c];
        }

        const auto row = [&](const int red, const int green, const int blue) -> const std::array<double, 3> & {
            // Independent red-fastest address expression: r + N * (g + N * b).
            const qsizetype index = red + qsizetype(cubeSize)
                * (green + qsizetype(cubeSize) * blue);
            return cubeRows.at(index);
        };
        std::array<double, 3> output {};
        for (int channel = 0; channel < 3; ++channel) {
            const std::size_t c = static_cast<std::size_t>(channel);
            const double c000 = row(lower[0], lower[1], lower[2])[c];
            const double c100 = row(upper[0], lower[1], lower[2])[c];
            const double c010 = row(lower[0], upper[1], lower[2])[c];
            const double c110 = row(upper[0], upper[1], lower[2])[c];
            const double c001 = row(lower[0], lower[1], upper[2])[c];
            const double c101 = row(upper[0], lower[1], upper[2])[c];
            const double c011 = row(lower[0], upper[1], upper[2])[c];
            const double c111 = row(upper[0], upper[1], upper[2])[c];
            const double x00 = c000 + (c100 - c000) * fraction[0];
            const double x10 = c010 + (c110 - c010) * fraction[0];
            const double x01 = c001 + (c101 - c001) * fraction[0];
            const double x11 = c011 + (c111 - c011) * fraction[0];
            const double y0 = x00 + (x10 - x00) * fraction[1];
            const double y1 = x01 + (x11 - x01) * fraction[1];
            output[c] = y0 + (y1 - y0) * fraction[2];
        }
        return output;
    }
};

std::array<double, 3> arrayFromJson(const QJsonValue &value, bool *ok = nullptr)
{
    if (ok) {
        *ok = false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() != 3) {
        return {};
    }
    std::array<double, 3> result {};
    for (int channel = 0; channel < 3; ++channel) {
        const QJsonValue component = array.at(channel);
        if (!component.isDouble() || !std::isfinite(component.toDouble())) {
            return {};
        }
        result[static_cast<std::size_t>(channel)] = component.toDouble();
    }
    if (ok) {
        *ok = true;
    }
    return result;
}

bool arraysClose(const std::array<double, 3> &left,
                 const std::array<double, 3> &right,
                 const double tolerance = ScalarTolerance)
{
    for (int channel = 0; channel < 3; ++channel) {
        if (std::abs(left[static_cast<std::size_t>(channel)]
                     - right[static_cast<std::size_t>(channel)]) > tolerance) {
            return false;
        }
    }
    return true;
}

QString arrayDescription(const std::array<double, 3> &value)
{
    return QStringLiteral("[%1, %2, %3]")
        .arg(value[0], 0, 'g', 12)
        .arg(value[1], 0, 'g', 12)
        .arg(value[2], 0, 'g', 12);
}

QJsonArray loadManifestCases(QString *error)
{
    if (error) {
        error->clear();
    }
    QFile file(fixturePath(QStringLiteral("vectors.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Could not open vectors.json");
        }
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("Invalid vectors.json: %1").arg(parseError.errorString());
        }
        return {};
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toInt() != 1
        || !root.value(QStringLiteral("cases")).isArray()) {
        if (error) {
            *error = QStringLiteral("Unsupported vectors.json schema");
        }
        return {};
    }
    return root.value(QStringLiteral("cases")).toArray();
}

} // namespace

class LutConformanceTests final : public QObject {
    Q_OBJECT

private slots:
    void independentReferenceMatchesAuthoritativeVectors();
    void productionMatchesDocumentedBaseline();
    void unclampedOutputAndStrengthBlendAreAuthoritative();
    void trilinearAndTetrahedralInterpolationAreDeterministic();
    void processingContractsAreExplicitAndStrengthBlendsInDocumentSpace();
    void processingModeGpuEligibilityTracksDocumentTransfer();
    void specialisedOperatorPipelinesMatchReferenceMath();
    void operatorProfilesAutoDetectPersistAndMigrate();
    void operatorProfilesEightAndSixteenBitRemainConsistent();
    void wideFiniteValuesAndDomainsArePreserved();
    void redFastestOrderingIsExplicitlyAsymmetric();
    void parserRejectsDuplicateConflictingAndUnsupportedDirectives();
    void parserErrorsIdentifyDirectiveAndLine();
    void domainSourceMetadataSurvivesPersistenceAndLegacyMigration();
    void floatingPointGpuTexturePreservesPrecisionAndRange();
    void gpuFallbackReasonsAreSpecificAndRecoverable();
    void lutSettingsSurviveDuplicationAndSessionResidency();
    void oversizedPresetFilesAreRejectedBeforeParsing();
    void presetOverwriteRequiresExplicitConsent();
    void cpuEightAndSixteenBitSwapRemainConsistent();
    void cpuTetrahedralEightAndSixteenBitRemainConsistent();
    void linearProcessingEightAndSixteenBitRemainConsistent();
    void integerDestinationsClampExtendedOutputWithoutWrapping();
    void fixtureDataSurvivesJsonProjectAndPresetPersistence();
};

void LutConformanceTests::independentReferenceMatchesAuthoritativeVectors()
{
    QString error;
    const QJsonArray cases = loadManifestCases(&error);
    QVERIFY2(!cases.isEmpty(), qPrintable(error));
    for (const QJsonValue &caseValue : cases) {
        const QJsonObject testCase = caseValue.toObject();
        const QString name = testCase.value(QStringLiteral("name")).toString();
        ReferenceLut reference;
        QVERIFY2(reference.parseFile(
                     fixturePath(testCase.value(QStringLiteral("fixture")).toString()),
                     &error),
                 qPrintable(QStringLiteral("%1: %2").arg(name, error)));
        const QJsonArray vectors = testCase.value(QStringLiteral("vectors")).toArray();
        QVERIFY2(!vectors.isEmpty(), qPrintable(name));
        for (const QJsonValue &vectorValue : vectors) {
            const QJsonObject vector = vectorValue.toObject();
            bool inputOk = false;
            bool expectedOk = false;
            const auto input = arrayFromJson(vector.value(QStringLiteral("input")), &inputOk);
            const auto expected = arrayFromJson(vector.value(QStringLiteral("expected")), &expectedOk);
            QVERIFY2(inputOk && expectedOk, qPrintable(name));
            const auto actual = reference.evaluate(input);
            QVERIFY2(arraysClose(actual, expected),
                     qPrintable(QStringLiteral("%1 reference expected %2, got %3")
                                    .arg(name,
                                         arrayDescription(expected),
                                         arrayDescription(actual))));
        }
    }
}

void LutConformanceTests::productionMatchesDocumentedBaseline()
{
    QString error;
    const QJsonArray cases = loadManifestCases(&error);
    QVERIFY2(!cases.isEmpty(), qPrintable(error));
    for (const QJsonValue &caseValue : cases) {
        const QJsonObject testCase = caseValue.toObject();
        if (testCase.value(QStringLiteral("mode")).toString()
            != QStringLiteral("matches-current")) {
            continue;
        }
        const QString name = testCase.value(QStringLiteral("name")).toString();
        LutParameters parameters;
        QVERIFY2(CubeLut::loadFile(
                     fixturePath(testCase.value(QStringLiteral("fixture")).toString()),
                     &parameters, &error),
                 qPrintable(QStringLiteral("%1: %2").arg(name, error)));
        for (const QJsonValue &vectorValue
             : testCase.value(QStringLiteral("vectors")).toArray()) {
            const QJsonObject vector = vectorValue.toObject();
            bool inputOk = false;
            bool expectedOk = false;
            const auto input = arrayFromJson(vector.value(QStringLiteral("input")), &inputOk);
            const auto expected = arrayFromJson(vector.value(QStringLiteral("expected")), &expectedOk);
            QVERIFY2(inputOk && expectedOk, qPrintable(name));
            const auto actual = CubeLut::evaluate(parameters, input);
            QVERIFY2(arraysClose(actual, expected),
                     qPrintable(QStringLiteral("%1 production expected %2, got %3")
                                    .arg(name,
                                         arrayDescription(expected),
                                         arrayDescription(actual))));
        }
    }
}

void LutConformanceTests::unclampedOutputAndStrengthBlendAreAuthoritative()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("extended_range_1d.cube")),
                 &parameters, &error),
             qPrintable(error));
    QCOMPARE(parameters.interpolation, LutInterpolation::Tetrahedral);

    const auto low = CubeLut::evaluate(parameters, {0.0, 0.5, 1.0});
    const auto high = CubeLut::evaluate(parameters, {1.0, 0.5, 0.0});
    QVERIFY(arraysClose(low, {-0.25, 0.5, 1.0}));
    QVERIFY(arraysClose(high, {1.25, 0.5, 0.0}));

    parameters.strength = 50.0;
    parameters.normalise();
    const auto half = CubeLut::evaluate(parameters, {0.0, 0.5, 1.0});
    QVERIFY(arraysClose(half, {-0.125, 0.5, 1.0}));
}

void LutConformanceTests::trilinearAndTetrahedralInterpolationAreDeterministic()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("interpolation_probe_3d.cube")),
                 &parameters, &error),
             qPrintable(error));
    QCOMPARE(parameters.interpolation, LutInterpolation::Tetrahedral);

    struct Probe {
        std::array<double, 3> input;
        std::array<double, 3> trilinear;
        std::array<double, 3> tetrahedral;
    };
    const QVector<Probe> probes {
        {{0.8, 0.4, 0.2}, {0.804, 0.424, 0.264}, {0.8, 0.4, 0.25}},
        {{0.2, 0.8, 0.4}, {0.264, 0.804, 0.424}, {0.25, 0.8, 0.4}},
        {{0.4, 0.2, 0.8}, {0.424, 0.264, 0.804}, {0.4, 0.25, 0.8}},
        // Exact fraction ties pin branch precedence and arithmetic order.
        {{0.5, 0.5, 0.25}, {0.515625, 0.515625, 0.296875}, {0.5, 0.5, 0.3125}},
        {{0.25, 0.5, 0.5}, {0.296875, 0.515625, 0.515625}, {0.3125, 0.5, 0.5}},
        {{0.5, 0.25, 0.5}, {0.515625, 0.296875, 0.515625}, {0.5, 0.3125, 0.5}}
    };

    parameters.interpolation = LutInterpolation::Trilinear;
    for (const Probe &probe : probes) {
        const auto actual = CubeLut::evaluate(parameters, probe.input);
        QVERIFY2(arraysClose(actual, probe.trilinear),
                 qPrintable(QStringLiteral("Trilinear expected %1, got %2")
                                .arg(arrayDescription(probe.trilinear),
                                     arrayDescription(actual))));
    }

    parameters.interpolation = LutInterpolation::Tetrahedral;
    for (const Probe &probe : probes) {
        const auto actual = CubeLut::evaluate(parameters, probe.input);
        QVERIFY2(arraysClose(actual, probe.tetrahedral),
                 qPrintable(QStringLiteral("Tetrahedral expected %1, got %2")
                                .arg(arrayDescription(probe.tetrahedral),
                                     arrayDescription(actual))));
    }

    const LutGpuTextureData tetrahedralTexture =
        CubeLut::buildGpuTextureData(parameters, &error);
    QVERIFY2(tetrahedralTexture.isValid(), qPrintable(error));
    parameters.interpolation = LutInterpolation::Trilinear;
    const LutGpuTextureData trilinearTexture =
        CubeLut::buildGpuTextureData(parameters, &error);
    QVERIFY2(trilinearTexture.isValid(), qPrintable(error));
    QVERIFY(tetrahedralTexture.rgba16f == trilinearTexture.rgba16f);
}


void LutConformanceTests::processingContractsAreExplicitAndStrengthBlendsInDocumentSpace()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("invert_1d.cube")),
                 &parameters, &error),
             qPrintable(error));
    parameters.strength = 100.0;

    const std::array<double, 3> encodedInput {0.25, 0.5, 0.75};
    parameters.processingMode = LutProcessingMode::EncodedDocument;
    QVERIFY(arraysClose(
        CubeLut::evaluate(parameters, encodedInput, LutDocumentTransfer::EncodedSrgb),
        {0.75, 0.5, 0.25}));

    parameters.processingMode = LutProcessingMode::RawComponents;
    QVERIFY(arraysClose(
        CubeLut::evaluate(parameters, encodedInput, LutDocumentTransfer::EncodedSrgb),
        {0.75, 0.5, 0.25}));

    parameters.processingMode = LutProcessingMode::LinearSrgb;
    std::array<double, 3> expectedLinearTableOnEncoded {};
    for (int channel = 0; channel < 3; ++channel) {
        const double linear = encodedToLinearExtended(encodedInput[channel]);
        expectedLinearTableOnEncoded[channel] =
            linearToEncodedExtended(1.0 - linear);
    }
    QVERIFY(arraysClose(
        CubeLut::evaluate(parameters, encodedInput, LutDocumentTransfer::EncodedSrgb),
        expectedLinearTableOnEncoded));

    const std::array<double, 3> linearDocumentInput {0.25, 0.5, 0.75};
    parameters.processingMode = LutProcessingMode::EncodedDocument;
    std::array<double, 3> expectedEncodedTableOnLinear {};
    for (int channel = 0; channel < 3; ++channel) {
        const double encoded = linearToEncodedExtended(linearDocumentInput[channel]);
        expectedEncodedTableOnLinear[channel] =
            encodedToLinearExtended(1.0 - encoded);
    }
    QVERIFY(arraysClose(
        CubeLut::evaluate(parameters, linearDocumentInput,
                          LutDocumentTransfer::LinearSrgb),
        expectedEncodedTableOnLinear));

    parameters.processingMode = LutProcessingMode::LinearSrgb;
    QVERIFY(arraysClose(
        CubeLut::evaluate(parameters, linearDocumentInput,
                          LutDocumentTransfer::LinearSrgb),
        {0.75, 0.5, 0.25}));

    parameters.strength = 50.0;
    const auto half = CubeLut::evaluate(
        parameters, linearDocumentInput, LutDocumentTransfer::LinearSrgb);
    QVERIFY(arraysClose(half, {0.5, 0.5, 0.5}));
}

void LutConformanceTests::processingModeGpuEligibilityTracksDocumentTransfer()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("invert_1d.cube")),
                 &parameters, &error),
             qPrintable(error));
    parameters.interpolation = LutInterpolation::Trilinear;
    parameters.normalise();

    const QColorSpace encoded(QColorSpace::SRgb);
    const QColorSpace linear(QColorSpace::SRgbLinear);
    const QColorSpace semanticEncoded(
        QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::SRgb);
    const QColorSpace semanticLinear(
        QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Linear);
    QCOMPARE(CubeLut::documentTransferFor(QColorSpace()),
             LutDocumentTransfer::EncodedSrgb);
    QCOMPARE(CubeLut::documentTransferFor(semanticEncoded),
             LutDocumentTransfer::EncodedSrgb);
    QCOMPARE(CubeLut::documentTransferFor(semanticLinear),
             LutDocumentTransfer::LinearSrgb);

    parameters.processingMode = LutProcessingMode::RawComponents;
    QVERIFY(!CubeLut::requiresCpuEvaluation(parameters, encoded));
    QVERIFY(!CubeLut::requiresCpuEvaluation(parameters, linear));

    parameters.processingMode = LutProcessingMode::EncodedDocument;
    QVERIFY(!CubeLut::requiresCpuEvaluation(parameters, encoded));
    QVERIFY(!CubeLut::requiresCpuEvaluation(parameters, linear));

    parameters.processingMode = LutProcessingMode::LinearSrgb;
    QVERIFY(!CubeLut::requiresCpuEvaluation(parameters, encoded));
    QVERIFY(!CubeLut::requiresCpuEvaluation(parameters, linear));

    const QColorSpace adobeRgb(QColorSpace::AdobeRgb);
    QVERIFY(!CubeLut::processingWarning(parameters, adobeRgb).isEmpty());
    parameters.processingMode = LutProcessingMode::RawComponents;
    QVERIFY(CubeLut::processingWarning(parameters, adobeRgb).isEmpty());

    LutParameters cube;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("interpolation_probe_3d.cube")),
                 &cube, &error),
             qPrintable(error));
    QCOMPARE(cube.interpolation, LutInterpolation::Tetrahedral);
    QVERIFY(!CubeLut::requiresCpuEvaluation(cube, linear));
}

void LutConformanceTests::specialisedOperatorPipelinesMatchReferenceMath()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("identity_3d.cube")),
                 &parameters, &error),
             qPrintable(error));
    const std::array<double, 3> encodedInput {0.18, 0.5, 0.9};

    parameters.operatorProfile = LutOperatorProfile::TonyMcMapface;
    parameters.strength = 100.0;
    parameters.normalise();
    QCOMPARE(parameters.interpolation, LutInterpolation::Tetrahedral);
    const auto tonyExpected = referenceTonyIdentity(
        encodedInput, LutDocumentTransfer::EncodedSrgb);
    const auto tonyActual = CubeLut::evaluate(
        parameters, encodedInput, LutDocumentTransfer::EncodedSrgb);
    QVERIFY2(arraysClose(tonyActual, tonyExpected),
             qPrintable(QStringLiteral("Tony expected %1, got %2")
                            .arg(arrayDescription(tonyExpected),
                                 arrayDescription(tonyActual))));
    QVERIFY(!CubeLut::requiresCpuEvaluation(
        parameters, QColorSpace(QColorSpace::SRgb)));
    QVERIFY2(CubeLut::buildGpuTextureData(parameters, &error).isValid(),
             qPrintable(error));

    parameters.strength = 50.0;
    parameters.normalise();
    const auto tonyHalf = CubeLut::evaluate(
        parameters, encodedInput, LutDocumentTransfer::EncodedSrgb);
    for (int channel = 0; channel < 3; ++channel) {
        const double expected = encodedInput[static_cast<std::size_t>(channel)]
            + (tonyExpected[static_cast<std::size_t>(channel)]
               - encodedInput[static_cast<std::size_t>(channel)]) * 0.5;
        QVERIFY(std::abs(tonyHalf[static_cast<std::size_t>(channel)] - expected)
                <= ScalarTolerance);
    }

    parameters.operatorProfile = LutOperatorProfile::AgXBaseSrgb;
    parameters.strength = 100.0;
    parameters.normalise();
    const auto agxExpected = referenceAgXIdentity(
        encodedInput, LutDocumentTransfer::EncodedSrgb);
    const auto agxActual = CubeLut::evaluate(
        parameters, encodedInput, LutDocumentTransfer::EncodedSrgb);
    QVERIFY2(arraysClose(agxActual, agxExpected),
             qPrintable(QStringLiteral("AgX expected %1, got %2")
                            .arg(arrayDescription(agxExpected),
                                 arrayDescription(agxActual))));

    const std::array<double, 3> linearInput {0.02, 0.18, 1.0};
    const auto agxLinearExpected = referenceAgXIdentity(
        linearInput, LutDocumentTransfer::LinearSrgb);
    const auto agxLinearActual = CubeLut::evaluate(
        parameters, linearInput, LutDocumentTransfer::LinearSrgb);
    QVERIFY2(arraysClose(agxLinearActual, agxLinearExpected),
             qPrintable(QStringLiteral("Linear AgX expected %1, got %2")
                            .arg(arrayDescription(agxLinearExpected),
                                 arrayDescription(agxLinearActual))));
}

void LutConformanceTests::operatorProfilesAutoDetectPersistAndMigrate()
{
    QFile fixture(fixturePath(QStringLiteral("identity_3d.cube")));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QByteArray contents = fixture.readAll();
    QString error;

    LutParameters tony;
    QVERIFY2(CubeLut::parse(contents, QStringLiteral("tony_mc_mapface.cube"),
                            &tony, &error), qPrintable(error));
    QCOMPARE(tony.operatorProfile, LutOperatorProfile::TonyMcMapface);
    QCOMPARE(tony.interpolation, LutInterpolation::Tetrahedral);

    LutParameters agx;
    QVERIFY2(CubeLut::parse(contents, QStringLiteral("AgX_Base_sRGB.cube"),
                            &agx, &error), qPrintable(error));
    QCOMPARE(agx.operatorProfile, LutOperatorProfile::AgXBaseSrgb);

    LutParameters generic;
    QVERIFY2(CubeLut::parse(contents, QStringLiteral("creative-look.cube"),
                            &generic, &error), qPrintable(error));
    QCOMPARE(generic.operatorProfile, LutOperatorProfile::Generic);

    QByteArray tonyTitleContents = contents;
    tonyTitleContents.replace("TITLE \"Identity 3D\"",
                              "TITLE \"Tony McMapface\"");
    LutParameters tonyFromTitle;
    QVERIFY2(CubeLut::parse(tonyTitleContents,
                            QStringLiteral("unrelated-name.cube"),
                            &tonyFromTitle, &error), qPrintable(error));
    QCOMPARE(tonyFromTitle.operatorProfile, LutOperatorProfile::TonyMcMapface);

    QByteArray agxTitleContents = contents;
    agxTitleContents.replace("TITLE \"Identity 3D\"",
                             "TITLE \"AgX Base sRGB\"");
    LutParameters agxFromTitle;
    QVERIFY2(CubeLut::parse(agxTitleContents,
                            QStringLiteral("unrelated-name.cube"),
                            &agxFromTitle, &error), qPrintable(error));
    QCOMPARE(agxFromTitle.operatorProfile, LutOperatorProfile::AgXBaseSrgb);

    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Lut);
    adjustment.parameters = tony;
    adjustment.normalise();
    bool encodedOk = false;
    const QJsonObject encoded = adjustment.toJson(&encodedOk);
    QVERIFY(encodedOk);
    QCOMPARE(encoded.value(QStringLiteral("schema")).toInt(),
             static_cast<int>(AdjustmentData::CurrentSchema));
    QCOMPARE(encoded.value(QStringLiteral("parameters")).toObject()
                 .value(QStringLiteral("operatorProfile")).toString(),
             QStringLiteral("tony-mc-mapface"));

    bool decodedOk = false;
    const AdjustmentData decoded = AdjustmentData::fromJson(
        encoded, AdjustmentType::Lut, &decodedOk);
    QVERIFY(decodedOk);
    QCOMPARE(std::get<LutParameters>(decoded.parameters).operatorProfile,
             LutOperatorProfile::TonyMcMapface);

    QJsonObject schemaNine = encoded;
    schemaNine.insert(QStringLiteral("schema"), 9);
    QJsonObject schemaNinePayload = schemaNine.value(
        QStringLiteral("parameters")).toObject();
    schemaNinePayload.remove(QStringLiteral("operatorProfile"));
    schemaNine.insert(QStringLiteral("parameters"), schemaNinePayload);
    bool migratedOk = false;
    const AdjustmentData migrated = AdjustmentData::fromJson(
        schemaNine, AdjustmentType::Lut, &migratedOk);
    QVERIFY(migratedOk);
    QCOMPARE(std::get<LutParameters>(migrated.parameters).operatorProfile,
             LutOperatorProfile::Generic);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage source(2, 1, QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    auto *sourcePixels = reinterpret_cast<QRgba64 *>(source.bits());
    sourcePixels[0] = QRgba64::fromRgba64(11'111, 22'222, 44'444, 0);
    sourcePixels[1] = QRgba64::fromRgba64(52'000, 31'000, 7'000, 43'210);

    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("operator-profile-source.png"));
    const QUuid layerId = document.addAdjustment(AdjustmentType::Lut);
    QVERIFY(!layerId.isNull());
    QVERIFY(document.updateLayer(layerId, [tony](LayerNode &layer) {
        layer.setLutParameters(tony);
    }));
    const QImage beforeSave = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers()).convertToFormat(QImage::Format_RGBA64);
    QVERIFY(!beforeSave.isNull());

    const QString projectPath = directory.filePath(
        QStringLiteral("operator-profile.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    const LutParameters restoredParameters = std::get<LutParameters>(
        restored.layerById(layerId).effectiveAdjustmentData().parameters);
    QCOMPARE(restoredParameters.operatorProfile, LutOperatorProfile::TonyMcMapface);
    QCOMPARE(restoredParameters.interpolation, LutInterpolation::Tetrahedral);
    const QImage afterReopen = ImageProcessor::renderPreservingHiddenRgb(
        restored.sourceImage(), restored.layers()).convertToFormat(QImage::Format_RGBA64);
    QCOMPARE(afterReopen, beforeSave);
}

void LutConformanceTests::operatorProfilesEightAndSixteenBitRemainConsistent()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("identity_3d.cube")),
                 &parameters, &error),
             qPrintable(error));

    const std::array<LutOperatorProfile, 2> profiles {
        LutOperatorProfile::TonyMcMapface,
        LutOperatorProfile::AgXBaseSrgb
    };
    for (const LutOperatorProfile profile : profiles) {
        parameters.operatorProfile = profile;
        parameters.strength = 100.0;
        parameters.normalise();

        LayerNode adjustment;
        adjustment.type = LayerType::Adjustment;
        adjustment.setLutParameters(parameters);

        QImage source8(2, 1, QImage::Format_RGBA8888);
        source8.setColorSpace(QColorSpace(QColorSpace::SRgb));
        source8.setPixelColor(0, 0, QColor(64, 128, 192, 0));
        source8.setPixelColor(1, 0, QColor(211, 73, 19, 173));
        LayerNode base8;
        base8.type = LayerType::BaseImage;
        base8.rasterImage = source8;
        base8.rasterReferenceSize = source8.size();
        const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
            source8, {adjustment, base8}).convertToFormat(QImage::Format_RGBA8888);

        QImage source16(2, 1, QImage::Format_RGBA64);
        source16.setColorSpace(QColorSpace(QColorSpace::SRgb));
        auto *source16Pixels = reinterpret_cast<QRgba64 *>(source16.bits());
        source16Pixels[0] = QRgba64::fromRgba64(
            qRound(64.0 / 255.0 * 65535.0),
            qRound(128.0 / 255.0 * 65535.0),
            qRound(192.0 / 255.0 * 65535.0),
            0);
        source16Pixels[1] = QRgba64::fromRgba64(
            qRound(211.0 / 255.0 * 65535.0),
            qRound(73.0 / 255.0 * 65535.0),
            qRound(19.0 / 255.0 * 65535.0),
            qRound(173.0 / 255.0 * 65535.0));
        LayerNode base16;
        base16.type = LayerType::BaseImage;
        base16.rasterImage = source16;
        base16.rasterReferenceSize = source16.size();
        const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
            source16, {adjustment, base16}).convertToFormat(QImage::Format_RGBA64);
        const auto *result16 = reinterpret_cast<const QRgba64 *>(output16.constBits());

        for (int pixel = 0; pixel < 2; ++pixel) {
            const QColor result8 = output8.pixelColor(pixel, 0);
            QVERIFY(std::abs(result8.redF() - result16[pixel].red() / 65535.0)
                    <= 1.0 / 255.0);
            QVERIFY(std::abs(result8.greenF() - result16[pixel].green() / 65535.0)
                    <= 1.0 / 255.0);
            QVERIFY(std::abs(result8.blueF() - result16[pixel].blue() / 65535.0)
                    <= 1.0 / 255.0);
        }
        QCOMPARE(output8.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(result16[0].alpha(), quint16(0));
        QCOMPARE(output8.pixelColor(1, 0).alpha(), 173);
        QVERIFY(std::abs(int(result16[1].alpha())
                         - qRound(173.0 / 255.0 * 65535.0)) <= 1);
    }
}

void LutConformanceTests::wideFiniteValuesAndDomainsArePreserved()
{
    QString error;
    LutParameters wideValues;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("wide_finite_values_1d.cube")),
                 &wideValues, &error),
             qPrintable(error));
    QCOMPARE(wideValues.shaperData.at(0), -32.0f);
    QCOMPARE(wideValues.shaperData.at(1), -24.0f);
    QCOMPARE(wideValues.shaperData.at(2), -20.0f);
    QCOMPARE(wideValues.shaperData.at(3), 32.0f);
    QCOMPARE(wideValues.shaperData.at(4), 24.0f);
    QCOMPARE(wideValues.shaperData.at(5), 20.0f);
    QVERIFY(!wideValues.gpuDisplayRangeCompatible);
    QVERIFY(wideValues.gpuHalfFloatCompatible);
    const LutGpuTextureData wideTexture =
        CubeLut::buildGpuTextureData(wideValues, &error);
    QVERIFY2(wideTexture.isValid(), qPrintable(error));
    QCOMPARE(float(wideTexture.rgba16f.at(0)), -32.0f);
    QCOMPARE(float(wideTexture.rgba16f.at(4)), 32.0f);

    LutParameters wideDomain;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("wide_domain_1d.cube")),
                 &wideDomain, &error),
             qPrintable(error));
    QVERIFY(wideDomain.shaperDomainMin
            == (std::array<double, 3> {-2048.0, -1024.0, -512.0}));
    QVERIFY(wideDomain.shaperDomainMax
            == (std::array<double, 3> {4096.0, 2048.0, 1024.0}));
    QCOMPARE(wideDomain.shaperDomainSource, LutDomainSource::DomainDirective);
    QCOMPARE(wideDomain.shaperData.at(0), -32.0f);
    QCOMPARE(wideDomain.shaperData.at(3), 32.0f);

    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Lut);
    adjustment.parameters = wideDomain;
    adjustment.normalise();
    bool encodedOk = false;
    const QJsonObject encoded = adjustment.toJson(&encodedOk);
    QVERIFY(encodedOk);
    bool decodedOk = false;
    const AdjustmentData decoded = AdjustmentData::fromJson(
        encoded, AdjustmentType::Lut, &decodedOk);
    QVERIFY(decodedOk);
    QVERIFY(decoded == adjustment);
}

void LutConformanceTests::redFastestOrderingIsExplicitlyAsymmetric()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("asymmetric_lattice_order_3d.cube")),
                 &parameters, &error),
             qPrintable(error));
    QCOMPARE(parameters.cubeSize, 2);
    QCOMPARE(parameters.cubeData.size(), 24);
    for (int row = 0; row < 8; ++row) {
        const double base = static_cast<double>(row) / 10.0;
        QVERIFY(std::abs(parameters.cubeData.at(row * 3) - base) < 1.0e-6);
        QVERIFY(std::abs(parameters.cubeData.at(row * 3 + 1) - (base + 0.01)) < 1.0e-6);
        QVERIFY(std::abs(parameters.cubeData.at(row * 3 + 2) - (base + 0.02)) < 1.0e-6);
    }

    const auto redCorner = CubeLut::evaluate(parameters, {1.0, 0.0, 0.0});
    const auto greenCorner = CubeLut::evaluate(parameters, {0.0, 1.0, 0.0});
    const auto blueCorner = CubeLut::evaluate(parameters, {0.0, 0.0, 1.0});
    QVERIFY(arraysClose(redCorner, {0.10, 0.11, 0.12}));
    QVERIFY(arraysClose(greenCorner, {0.20, 0.21, 0.22}));
    QVERIFY(arraysClose(blueCorner, {0.40, 0.41, 0.42}));
}

void LutConformanceTests::parserRejectsDuplicateConflictingAndUnsupportedDirectives()
{
    struct RejectionCase {
        QString fixture;
        QString directive;
        QString line;
    };
    const QVector<RejectionCase> cases {
        {QStringLiteral("invalid/duplicate_size.cube"),
         QStringLiteral("Duplicate LUT_3D_SIZE"), QStringLiteral("line 2")},
        {QStringLiteral("invalid/duplicate_range.cube"),
         QStringLiteral("Duplicate LUT_1D_INPUT_RANGE"), QStringLiteral("line 3")},
        {QStringLiteral("invalid/duplicate_domain.cube"),
         QStringLiteral("Duplicate DOMAIN_MIN"), QStringLiteral("line 3")},
        {QStringLiteral("invalid/duplicate_title.cube"),
         QStringLiteral("Duplicate TITLE"), QStringLiteral("line 2")},
        {QStringLiteral("invalid/conflicting_ranges.cube"),
         QStringLiteral("LUT_3D_INPUT_RANGE conflicts"), QStringLiteral("line 4")},
        {QStringLiteral("invalid/video_range.cube"),
         QStringLiteral("Unsupported LUT_IN_VIDEO_RANGE"), QStringLiteral("line 2")},
        {QStringLiteral("invalid/video_output_range.cube"),
         QStringLiteral("Unsupported LUT_OUT_VIDEO_RANGE"), QStringLiteral("line 2")},
        {QStringLiteral("invalid/unknown_directive.cube"),
         QStringLiteral("Unsupported LUT directive 'LUT_MAGIC_MODE'"), QStringLiteral("line 2")},
        {QStringLiteral("invalid/range_without_size.cube"),
         QStringLiteral("LUT_3D_INPUT_RANGE was declared without LUT_3D_SIZE"),
         QStringLiteral("line 1")}
    };

    for (const RejectionCase &testCase : cases) {
        LutParameters parameters;
        QString error;
        QVERIFY2(!CubeLut::loadFile(fixturePath(testCase.fixture), &parameters, &error),
                 qPrintable(testCase.fixture));
        QVERIFY2(error.contains(testCase.directive), qPrintable(error));
        QVERIFY2(error.contains(testCase.line), qPrintable(error));
        QVERIFY(!parameters.hasData());
    }
}

void LutConformanceTests::parserErrorsIdentifyDirectiveAndLine()
{
    struct RejectionCase {
        QString fixture;
        QString errorFragment;
    };
    const QVector<RejectionCase> cases {
        {QStringLiteral("invalid/directive_after_data.cube"),
         QStringLiteral("after table data began at line 3")},
        {QStringLiteral("invalid/extra_rows.cube"),
         QStringLiteral("Unexpected extra LUT data row at line 4")},
        {QStringLiteral("invalid/missing_rows.cube"),
         QStringLiteral("LUT_3D_SIZE declares 8 data rows but only 2 were provided")}
    };
    for (const RejectionCase &testCase : cases) {
        LutParameters parameters;
        QString error;
        QVERIFY(!CubeLut::loadFile(fixturePath(testCase.fixture), &parameters, &error));
        QVERIFY2(error.contains(testCase.errorFragment), qPrintable(error));
    }

    LutParameters parameters;
    QString error;
    const QByteArray invalidData = QByteArrayLiteral(
        "LUT_1D_SIZE 2\n"
        "0 0 0\n"
        "1 nope 1\n");
    QVERIFY(!CubeLut::parse(invalidData, QStringLiteral("invalid-data.cube"),
                            &parameters, &error));
    QVERIFY2(error.contains(QStringLiteral("Invalid LUT data row at line 3")),
             qPrintable(error));
}

void LutConformanceTests::domainSourceMetadataSurvivesPersistenceAndLegacyMigration()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("specific_input_ranges_combined.cube")),
                 &parameters, &error),
             qPrintable(error));
    QCOMPARE(parameters.shaperDomainSource, LutDomainSource::InputRangeDirective);
    QCOMPARE(parameters.cubeDomainSource, LutDomainSource::InputRangeDirective);
    QVERIFY(parameters.shaperDomainMin
            == (std::array<double, 3> {-2.0, -2.0, -2.0}));
    QVERIFY(parameters.shaperDomainMax
            == (std::array<double, 3> {2.0, 2.0, 2.0}));

    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Lut);
    adjustment.parameters = parameters;
    adjustment.normalise();
    bool encodedOk = false;
    QJsonObject encoded = adjustment.toJson(&encodedOk);
    QVERIFY(encodedOk);
    QCOMPARE(encoded.value(QStringLiteral("schema")).toInt(),
             static_cast<int>(AdjustmentData::CurrentSchema));
    const QJsonObject payload = encoded.value(QStringLiteral("parameters")).toObject();
    QCOMPARE(payload.value(QStringLiteral("shaperDomainSource")).toString(),
             QStringLiteral("input-range-directive"));
    QCOMPARE(payload.value(QStringLiteral("cubeDomainSource")).toString(),
             QStringLiteral("input-range-directive"));
    QCOMPARE(payload.value(QStringLiteral("interpolation")).toString(),
             QStringLiteral("tetrahedral"));
    QCOMPARE(payload.value(QStringLiteral("processingMode")).toString(),
             QStringLiteral("encoded-document"));
    QCOMPARE(payload.value(QStringLiteral("operatorProfile")).toString(),
             QStringLiteral("generic"));

    bool decodedOk = false;
    const AdjustmentData decoded = AdjustmentData::fromJson(
        encoded, AdjustmentType::Lut, &decodedOk);
    QVERIFY(decodedOk);
    QVERIFY(decoded == adjustment);

    QJsonObject legacy = encoded;
    legacy.insert(QStringLiteral("schema"), 7);
    QJsonObject legacyPayload = legacy.value(QStringLiteral("parameters")).toObject();
    legacyPayload.remove(QStringLiteral("interpolation"));
    legacyPayload.remove(QStringLiteral("processingMode"));
    legacyPayload.remove(QStringLiteral("operatorProfile"));
    legacy.insert(QStringLiteral("parameters"), legacyPayload);
    bool legacyOk = false;
    const AdjustmentData migrated = AdjustmentData::fromJson(
        legacy, AdjustmentType::Lut, &legacyOk);
    QVERIFY(legacyOk);
    const LutParameters migratedParameters = std::get<LutParameters>(migrated.parameters);
    QCOMPARE(migratedParameters.shaperDomainSource, LutDomainSource::InputRangeDirective);
    QCOMPARE(migratedParameters.cubeDomainSource, LutDomainSource::InputRangeDirective);
    QCOMPARE(migratedParameters.interpolation, LutInterpolation::Trilinear);
    QCOMPARE(migratedParameters.processingMode, LutProcessingMode::EncodedDocument);
    QVERIFY(migratedParameters.shaperDomainMin == parameters.shaperDomainMin);
    QVERIFY(migratedParameters.cubeDomainMin == parameters.cubeDomainMin);

    QJsonObject schemaSix = encoded;
    schemaSix.insert(QStringLiteral("schema"), 6);
    QJsonObject schemaSixPayload = schemaSix.value(QStringLiteral("parameters")).toObject();
    schemaSixPayload.remove(QStringLiteral("shaperDomainSource"));
    schemaSixPayload.remove(QStringLiteral("cubeDomainSource"));
    schemaSixPayload.remove(QStringLiteral("interpolation"));
    schemaSixPayload.remove(QStringLiteral("processingMode"));
    schemaSixPayload.remove(QStringLiteral("operatorProfile"));
    schemaSix.insert(QStringLiteral("parameters"), schemaSixPayload);
    bool schemaSixOk = false;
    const AdjustmentData migratedSchemaSix = AdjustmentData::fromJson(
        schemaSix, AdjustmentType::Lut, &schemaSixOk);
    QVERIFY(schemaSixOk);
    const LutParameters schemaSixParameters = std::get<LutParameters>(
        migratedSchemaSix.parameters);
    QCOMPARE(schemaSixParameters.shaperDomainSource, LutDomainSource::LegacyPersisted);
    QCOMPARE(schemaSixParameters.cubeDomainSource, LutDomainSource::DefaultRange);
    QCOMPARE(schemaSixParameters.interpolation, LutInterpolation::Trilinear);
    QCOMPARE(schemaSixParameters.processingMode, LutProcessingMode::EncodedDocument);

    QJsonObject schemaEight = encoded;
    schemaEight.insert(QStringLiteral("schema"), 8);
    QJsonObject schemaEightPayload =
        schemaEight.value(QStringLiteral("parameters")).toObject();
    schemaEightPayload.remove(QStringLiteral("processingMode"));
    schemaEightPayload.remove(QStringLiteral("operatorProfile"));
    schemaEight.insert(QStringLiteral("parameters"), schemaEightPayload);
    bool schemaEightOk = false;
    const AdjustmentData migratedSchemaEight = AdjustmentData::fromJson(
        schemaEight, AdjustmentType::Lut, &schemaEightOk);
    QVERIFY(schemaEightOk);
    QCOMPARE(std::get<LutParameters>(migratedSchemaEight.parameters).processingMode,
             LutProcessingMode::EncodedDocument);

    QJsonObject invalid = encoded;
    QJsonObject invalidPayload = invalid.value(QStringLiteral("parameters")).toObject();
    QJsonArray invalidMaximum;
    invalidMaximum << -2.0 << -2.0 << -2.0;
    invalidPayload.insert(QStringLiteral("shaperDomainMax"), invalidMaximum);
    invalid.insert(QStringLiteral("parameters"), invalidPayload);
    bool invalidOk = true;
    AdjustmentData::fromJson(invalid, AdjustmentType::Lut, &invalidOk);
    QVERIFY(!invalidOk);
}

void LutConformanceTests::floatingPointGpuTexturePreservesPrecisionAndRange()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("fractional_precision_1d.cube")),
                 &parameters, &error),
             qPrintable(error));
    const LutGpuTextureData texture =
        CubeLut::buildGpuTextureData(parameters, &error);
    QVERIFY2(texture.isValid(), qPrintable(error));
    QCOMPARE(texture.shaperRow, 0);
    QCOMPARE(texture.size, QSize(2, 1));
    const float red = float(texture.rgba16f.at(0));
    const float green = float(texture.rgba16f.at(1));
    const float blue = float(texture.rgba16f.at(2));
    QVERIFY(std::abs(red - 0.123456f) < 1.0e-4f);
    QVERIFY(std::abs(green - 0.234567f) < 1.0e-4f);
    QVERIFY(std::abs(blue - 0.345678f) < 2.0e-4f);
    QVERIFY(std::abs(red - qRound(0.123456 * 255.0) / 255.0) > 1.0e-4f);

    LutParameters temporaryWideDomain = parameters;
    temporaryWideDomain.shaperDomainMax = {1.0e100, 1.0e100, 1.0e100};
    temporaryWideDomain.normalise();
    QVERIFY(!temporaryWideDomain.gpuHalfFloatCompatible);
    temporaryWideDomain.shaperDomainMax = {1.0, 1.0, 1.0};
    temporaryWideDomain.normalise();
    QVERIFY(temporaryWideDomain.gpuHalfFloatCompatible);

    LutParameters beyondHalf = parameters;
    beyondHalf.shaperData[0] = 70000.0f;
    beyondHalf.tableFingerprint = 0;
    beyondHalf.normalise();
    QVERIFY(!beyondHalf.gpuHalfFloatCompatible);
    beyondHalf.normalise();
    QVERIFY(!beyondHalf.gpuHalfFloatCompatible);
    QVERIFY(!CubeLut::buildGpuTextureData(beyondHalf, &error).isValid());
    QVERIFY(error.contains(QStringLiteral("RGBA16Float"), Qt::CaseInsensitive));
}


void LutConformanceTests::gpuFallbackReasonsAreSpecificAndRecoverable()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("identity_3d.cube")),
                 &parameters, &error),
             qPrintable(error));
    QVERIFY(CubeLut::gpuFallbackReason(parameters).isEmpty());
    QVERIFY(!CubeLut::requiresCpuEvaluation(
        parameters, QColorSpace(QColorSpace::SRgb)));

    LutParameters wideTable = parameters;
    wideTable.cubeData[0] = 70'000.0f;
    wideTable.tableFingerprint = 0;
    wideTable.normalise();
    const QString tableReason = CubeLut::gpuFallbackReason(wideTable);
    QVERIFY2(tableReason.contains(QStringLiteral("table samples"),
                                  Qt::CaseInsensitive),
             qPrintable(tableReason));
    QVERIFY(CubeLut::requiresCpuEvaluation(
        wideTable, QColorSpace(QColorSpace::SRgb)));

    LutParameters wideDomain = parameters;
    wideDomain.cubeDomainMax[0] = 1.0e100;
    wideDomain.cubeDomainSource = LutDomainSource::DomainDirective;
    wideDomain.normalise();
    const QString domainReason = CubeLut::gpuFallbackReason(wideDomain);
    QVERIFY2(domainReason.contains(QStringLiteral("input-domain"),
                                   Qt::CaseInsensitive),
             qPrintable(domainReason));
    QVERIFY(CubeLut::requiresCpuEvaluation(
        wideDomain, QColorSpace(QColorSpace::SRgb)));

    wideDomain.cubeDomainMax[0] = 1.0;
    wideDomain.normalise();
    QVERIFY(CubeLut::gpuFallbackReason(wideDomain).isEmpty());
    QVERIFY(!CubeLut::requiresCpuEvaluation(
        wideDomain, QColorSpace(QColorSpace::SRgb)));

    LutParameters oversizedTexture = parameters;
    oversizedTexture.shaperSize = 8193;
    oversizedTexture.shaperData.resize(oversizedTexture.shaperSize * 3);
    std::fill(oversizedTexture.shaperData.begin(),
              oversizedTexture.shaperData.end(), 0.5f);
    oversizedTexture.tableFingerprint = 0;
    oversizedTexture.normalise();
    const QString dimensionReason = CubeLut::gpuFallbackReason(oversizedTexture);
    QVERIFY2(dimensionReason.contains(QStringLiteral("8192")),
             qPrintable(dimensionReason));
    QString textureError;
    QVERIFY(!CubeLut::buildGpuTextureData(oversizedTexture, &textureError).isValid());
    QCOMPARE(textureError, dimensionReason);

    oversizedTexture.shaperSize = 0;
    oversizedTexture.shaperData.clear();
    oversizedTexture.tableFingerprint = 0;
    oversizedTexture.normalise();
    QVERIFY(CubeLut::gpuFallbackReason(oversizedTexture).isEmpty());
}

void LutConformanceTests::lutSettingsSurviveDuplicationAndSessionResidency()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("specific_input_ranges_combined.cube")),
                 &parameters, &error),
             qPrintable(error));
    parameters.interpolation = LutInterpolation::Trilinear;
    parameters.processingMode = LutProcessingMode::RawComponents;
    parameters.operatorProfile = LutOperatorProfile::Generic;
    parameters.strength = 42.0;
    parameters.normalise();

    QImage source(3, 2, QImage::Format_RGBA64);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(71, 123, 211, 137));

    DocumentSession session;
    session.document().setSourceImage(source, QStringLiteral("residency-lut.png"));
    const QUuid layerId = session.document().addAdjustment(AdjustmentType::Lut);
    QVERIFY(!layerId.isNull());
    QVERIFY(session.document().updateLayer(layerId, [parameters](LayerNode &layer) {
        layer.setLutParameters(parameters);
    }));

    const QVector<QUuid> duplicates = session.document().duplicateLayers({layerId});
    QCOMPARE(duplicates.size(), 1);
    const LutParameters duplicateParameters = std::get<LutParameters>(
        session.document().layerById(duplicates.front())
            .effectiveAdjustmentData().parameters);
    QVERIFY(duplicateParameters == parameters);

    const QImage beforeSnapshot = ImageProcessor::renderPreservingHiddenRgb(
        session.document().sourceImage(), session.document().layers())
                                      .convertToFormat(QImage::Format_RGBA64);
    QVERIFY(!beforeSnapshot.isNull());

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SessionCacheStore store(directory.filePath(QStringLiteral("cache")));
    QVERIFY(store.isAvailable());
    QString snapshotPath;
    QVERIFY2(store.writeSnapshot(session, &snapshotPath, nullptr, &error),
             qPrintable(error));
    QVERIFY(QFileInfo::exists(snapshotPath));

    DocumentSession restored;
    QVERIFY2(store.restoreSnapshot(snapshotPath, &restored, &error),
             qPrintable(error));
    const LutParameters restoredOriginal = std::get<LutParameters>(
        restored.document().layerById(layerId)
            .effectiveAdjustmentData().parameters);
    const LutParameters restoredDuplicate = std::get<LutParameters>(
        restored.document().layerById(duplicates.front())
            .effectiveAdjustmentData().parameters);
    QVERIFY(restoredOriginal == parameters);
    QVERIFY(restoredDuplicate == parameters);
    QCOMPARE(restoredOriginal.shaperDomainSource,
             LutDomainSource::InputRangeDirective);
    QCOMPARE(restoredOriginal.cubeDomainSource,
             LutDomainSource::InputRangeDirective);
    QCOMPARE(restoredOriginal.interpolation, LutInterpolation::Trilinear);
    QCOMPARE(restoredOriginal.processingMode, LutProcessingMode::RawComponents);
    QCOMPARE(restoredOriginal.strength, 42.0);

    const QImage afterSnapshot = ImageProcessor::renderPreservingHiddenRgb(
        restored.document().sourceImage(), restored.document().layers())
                                     .convertToFormat(QImage::Format_RGBA64);
    QCOMPARE(afterSnapshot, beforeSnapshot);
}

void LutConformanceTests::oversizedPresetFilesAreRejectedBeforeParsing()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString directoryPath = AdjustmentPresetStore::storageDirectory(
        AdjustmentType::Lut);
    QVERIFY(QDir().mkpath(directoryPath));
    const QString path = QDir(directoryPath).filePath(
        QStringLiteral("oversized-lut-preset-%1.json")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(33LL * 1024LL * 1024LL));
    file.close();

    QStringList warnings;
    const QVector<AdjustmentPreset> presets = AdjustmentPresetStore::presets(
        AdjustmentType::Lut, &warnings);
    Q_UNUSED(presets);
    QVERIFY2(std::any_of(warnings.cbegin(), warnings.cend(),
                         [&path](const QString &warning) {
        return warning.contains(QFileInfo(path).fileName())
            && warning.contains(QStringLiteral("oversized"),
                                Qt::CaseInsensitive);
    }), qPrintable(warnings.join(QStringLiteral("; "))));
    QVERIFY(QFile::remove(path));
}


void LutConformanceTests::presetOverwriteRequiresExplicitConsent()
{
    QStandardPaths::setTestModeEnabled(true);
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("identity_3d.cube")),
                 &parameters, &error),
             qPrintable(error));
    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Lut);
    adjustment.parameters = parameters;
    adjustment.normalise();

    const QString name = QStringLiteral("LUT overwrite %1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(!AdjustmentPresetStore::userPresetExists(
        name, AdjustmentType::Lut));
    QVERIFY2(AdjustmentPresetStore::saveUserPreset(name, adjustment, &error),
             qPrintable(error));
    QVERIFY(AdjustmentPresetStore::userPresetExists(
        name, AdjustmentType::Lut));

    error.clear();
    QVERIFY(!AdjustmentPresetStore::saveUserPreset(name, adjustment, &error));
    QVERIFY2(error.contains(QStringLiteral("already exists"),
                            Qt::CaseInsensitive),
             qPrintable(error));

    parameters.strength = 37.0;
    adjustment.parameters = parameters;
    adjustment.normalise();
    error.clear();
    QVERIFY2(AdjustmentPresetStore::saveUserPreset(
                 name, adjustment, &error, true),
             qPrintable(error));

    const QVector<AdjustmentPreset> presets = AdjustmentPresetStore::presets(
        AdjustmentType::Lut);
    const auto found = std::find_if(
        presets.cbegin(), presets.cend(), [&name](const AdjustmentPreset &preset) {
            return !preset.builtIn && preset.name == name;
        });
    QVERIFY(found != presets.cend());
    QCOMPARE(std::get<LutParameters>(found->adjustment.parameters).strength,
             37.0);
    const AdjustmentPreset saved = *found;
    QVERIFY2(AdjustmentPresetStore::removeUserPreset(saved, &error),
             qPrintable(error));
}

void LutConformanceTests::cpuEightAndSixteenBitSwapRemainConsistent()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("red_blue_swap_3d.cube")),
                 &parameters, &error),
             qPrintable(error));

    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    adjustment.setLutParameters(parameters);

    QImage source8(2, 1, QImage::Format_RGBA8888);
    source8.setPixelColor(0, 0, QColor(13, 77, 211, 0));
    source8.setPixelColor(1, 0, QColor(240, 9, 81, 173));
    LayerNode base8;
    base8.type = LayerType::BaseImage;
    base8.rasterImage = source8;
    base8.rasterReferenceSize = source8.size();
    const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {adjustment, base8}).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output8.pixelColor(0, 0), QColor(211, 77, 13, 0));
    QCOMPARE(output8.pixelColor(1, 0), QColor(81, 9, 240, 173));

    QImage source16(2, 1, QImage::Format_RGBA64);
    auto *sourcePixels = reinterpret_cast<QRgba64 *>(source16.bits());
    sourcePixels[0] = QRgba64::fromRgba64(1'337, 19'753, 61'121, 0);
    sourcePixels[1] = QRgba64::fromRgba64(63'001, 2'009, 31'337, 44'444);
    LayerNode base16;
    base16.type = LayerType::BaseImage;
    base16.rasterImage = source16;
    base16.rasterReferenceSize = source16.size();
    const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {adjustment, base16}).convertToFormat(QImage::Format_RGBA64);
    const auto *outputPixels = reinterpret_cast<const QRgba64 *>(output16.constBits());
    QCOMPARE(outputPixels[0], QRgba64::fromRgba64(61'121, 19'753, 1'337, 0));
    QCOMPARE(outputPixels[1], QRgba64::fromRgba64(31'337, 2'009, 63'001, 44'444));
}

void LutConformanceTests::cpuTetrahedralEightAndSixteenBitRemainConsistent()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("interpolation_probe_3d.cube")),
                 &parameters, &error),
             qPrintable(error));
    QCOMPARE(parameters.interpolation, LutInterpolation::Tetrahedral);

    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    adjustment.setLutParameters(parameters);

    QImage source8(1, 1, QImage::Format_RGBA8888);
    source8.setPixelColor(0, 0, QColor(204, 102, 51, 73));
    LayerNode base8;
    base8.type = LayerType::BaseImage;
    base8.rasterImage = source8;
    base8.rasterReferenceSize = source8.size();
    const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {adjustment, base8}).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output8.pixelColor(0, 0), QColor(204, 102, 64, 73));

    QImage source16(1, 1, QImage::Format_RGBA64);
    auto *sourcePixel = reinterpret_cast<QRgba64 *>(source16.bits());
    sourcePixel[0] = QRgba64::fromRgba64(52'428, 26'214, 13'107, 40'001);
    LayerNode base16;
    base16.type = LayerType::BaseImage;
    base16.rasterImage = source16;
    base16.rasterReferenceSize = source16.size();
    const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {adjustment, base16}).convertToFormat(QImage::Format_RGBA64);
    const auto *outputPixel = reinterpret_cast<const QRgba64 *>(output16.constBits());
    QCOMPARE(outputPixel[0], QRgba64::fromRgba64(52'428, 26'214, 16'384, 40'001));
}


void LutConformanceTests::linearProcessingEightAndSixteenBitRemainConsistent()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("invert_1d.cube")),
                 &parameters, &error),
             qPrintable(error));
    parameters.processingMode = LutProcessingMode::LinearSrgb;
    parameters.interpolation = LutInterpolation::Trilinear;
    parameters.normalise();

    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    adjustment.setLutParameters(parameters);

    QImage source8(1, 1, QImage::Format_RGBA8888);
    source8.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source8.setPixelColor(0, 0, QColor(64, 128, 192, 0));
    LayerNode base8;
    base8.type = LayerType::BaseImage;
    base8.rasterImage = source8;
    base8.rasterReferenceSize = source8.size();
    const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {adjustment, base8}).convertToFormat(QImage::Format_RGBA8888);

    QImage source16(1, 1, QImage::Format_RGBA64);
    source16.setColorSpace(QColorSpace(QColorSpace::SRgb));
    auto *source16Pixel = reinterpret_cast<QRgba64 *>(source16.bits());
    source16Pixel[0] = QRgba64::fromRgba64(
        qRound(64.0 / 255.0 * 65535.0),
        qRound(128.0 / 255.0 * 65535.0),
        qRound(192.0 / 255.0 * 65535.0),
        0);
    LayerNode base16;
    base16.type = LayerType::BaseImage;
    base16.rasterImage = source16;
    base16.rasterReferenceSize = source16.size();
    const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {adjustment, base16}).convertToFormat(QImage::Format_RGBA64);
    const QRgba64 result16 =
        reinterpret_cast<const QRgba64 *>(output16.constBits())[0];

    const QColor result8 = output8.pixelColor(0, 0);
    QVERIFY(std::abs(result8.redF() - result16.red() / 65535.0) <= 1.0 / 255.0);
    QVERIFY(std::abs(result8.greenF() - result16.green() / 65535.0) <= 1.0 / 255.0);
    QVERIFY(std::abs(result8.blueF() - result16.blue() / 65535.0) <= 1.0 / 255.0);
    QCOMPARE(result8.alpha(), 0);
    QCOMPARE(result16.alpha(), quint16(0));
}


void LutConformanceTests::integerDestinationsClampExtendedOutputWithoutWrapping()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("extended_range_1d.cube")),
                 &parameters, &error),
             qPrintable(error));
    parameters.processingMode = LutProcessingMode::RawComponents;
    parameters.normalise();

    LayerNode adjustment;
    adjustment.type = LayerType::Adjustment;
    adjustment.setLutParameters(parameters);

    QImage source8(2, 1, QImage::Format_RGBA8888);
    source8.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source8.setPixelColor(0, 0, QColor(0, 128, 255, 0));
    source8.setPixelColor(1, 0, QColor(255, 128, 0, 173));
    LayerNode base8;
    base8.type = LayerType::BaseImage;
    base8.rasterImage = source8;
    base8.rasterReferenceSize = source8.size();
    const QImage output8 = ImageProcessor::renderPreservingHiddenRgb(
        source8, {adjustment, base8}).convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(output8.pixelColor(0, 0), QColor(0, 128, 255, 0));
    QCOMPARE(output8.pixelColor(1, 0), QColor(255, 128, 0, 173));

    QImage source16(2, 1, QImage::Format_RGBA64);
    source16.setColorSpace(QColorSpace(QColorSpace::SRgb));
    auto *source16Pixel = reinterpret_cast<QRgba64 *>(source16.bits());
    source16Pixel[0] = QRgba64::fromRgba64(0, 32'768, 65'535, 0);
    source16Pixel[1] = QRgba64::fromRgba64(65'535, 32'768, 0, 44'444);
    LayerNode base16;
    base16.type = LayerType::BaseImage;
    base16.rasterImage = source16;
    base16.rasterReferenceSize = source16.size();
    const QImage output16 = ImageProcessor::renderPreservingHiddenRgb(
        source16, {adjustment, base16}).convertToFormat(QImage::Format_RGBA64);
    const auto *result16 =
        reinterpret_cast<const QRgba64 *>(output16.constBits());
    QCOMPARE(result16[0].red(), quint16(0));
    QVERIFY(std::abs(int(result16[0].green()) - 32'768) <= 1);
    QCOMPARE(result16[0].blue(), quint16(65'535));
    QCOMPARE(result16[0].alpha(), quint16(0));
    QCOMPARE(result16[1].red(), quint16(65'535));
    QVERIFY(std::abs(int(result16[1].green()) - 32'768) <= 1);
    QCOMPARE(result16[1].blue(), quint16(0));
    QCOMPARE(result16[1].alpha(), quint16(44'444));
}

void LutConformanceTests::fixtureDataSurvivesJsonProjectAndPresetPersistence()
{
    LutParameters parameters;
    QString error;
    QVERIFY2(CubeLut::loadFile(
                 fixturePath(QStringLiteral("combined_shaper_swap_3d.cube")),
                 &parameters, &error),
             qPrintable(error));
    parameters.strength = 63.0;
    parameters.processingMode = LutProcessingMode::LinearSrgb;
    parameters.normalise();

    AdjustmentData adjustment;
    adjustment.reset(AdjustmentType::Lut);
    adjustment.parameters = parameters;
    adjustment.normalise();
    bool jsonOk = false;
    const QJsonObject encoded = adjustment.toJson(&jsonOk);
    QVERIFY(jsonOk);
    bool decodedOk = false;
    const AdjustmentData decoded = AdjustmentData::fromJson(
        encoded, AdjustmentType::Lut, &decodedOk);
    QVERIFY(decodedOk);
    QVERIFY(decoded == adjustment);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage source(3, 2, QImage::Format_RGBA64);
    source.fill(QColor(37, 91, 183, 121));
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    PhotoDocument document;
    document.setSourceImage(source, QStringLiteral("fixture-source.png"));
    const QUuid layerId = document.addAdjustment(AdjustmentType::Lut);
    QVERIFY(!layerId.isNull());
    QVERIFY(document.updateLayer(layerId, [parameters](LayerNode &layer) {
        layer.setLutParameters(parameters);
    }));
    const QImage flattenedBeforeSave = ImageProcessor::renderPreservingHiddenRgb(
        document.sourceImage(), document.layers()).convertToFormat(QImage::Format_RGBA64);
    QVERIFY(!flattenedBeforeSave.isNull());

    const QString projectPath = directory.filePath(QStringLiteral("lut-baseline.vfxphoto"));
    QVERIFY2(document.saveProject(projectPath, &error), qPrintable(error));
    PhotoDocument restored;
    QVERIFY2(restored.loadProject(projectPath, &error), qPrintable(error));
    const LutParameters restoredParameters = std::get<LutParameters>(
        restored.layerById(layerId).effectiveAdjustmentData().parameters);
    QVERIFY(restoredParameters == parameters);
    const QImage flattenedAfterReopen = ImageProcessor::renderPreservingHiddenRgb(
        restored.sourceImage(), restored.layers()).convertToFormat(QImage::Format_RGBA64);
    QCOMPARE(flattenedAfterReopen, flattenedBeforeSave);

    QStandardPaths::setTestModeEnabled(true);
    const QString presetName = QStringLiteral("LUT Conformance %1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY2(AdjustmentPresetStore::saveUserPreset(presetName, adjustment, &error),
             qPrintable(error));
    const QVector<AdjustmentPreset> presets = AdjustmentPresetStore::presets(
        AdjustmentType::Lut);
    const auto found = std::find_if(
        presets.cbegin(), presets.cend(),
        [&presetName](const AdjustmentPreset &preset) {
            return !preset.builtIn && preset.name == presetName;
        });
    QVERIFY(found != presets.cend());
    QVERIFY(found->adjustment == adjustment);
    const AdjustmentPreset saved = *found;
    QVERIFY2(AdjustmentPresetStore::removeUserPreset(saved, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(saved.storagePath));
}

QTEST_GUILESS_MAIN(LutConformanceTests)
#include "test_lut_conformance.moc"
