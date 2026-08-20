#include "CubeLut.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vfx {
namespace {

constexpr int MaximumOneDSize = 65'536;
constexpr int MaximumThreeDSize = 65;

bool parseInteger(const QString &text, int *value)
{
    bool ok = false;
    const qlonglong decoded = text.toLongLong(&ok);
    if (!ok || decoded < 0 || decoded > std::numeric_limits<int>::max()) return false;
    if (value) *value = static_cast<int>(decoded);
    return true;
}

bool parseFinite(const QString &text, double *value)
{
    bool ok = false;
    const double decoded = text.toDouble(&ok);
    if (!ok || !std::isfinite(decoded)) return false;
    if (value) *value = decoded;
    return true;
}

QStringList tokensForLine(QString line)
{
    bool insideQuotes = false;
    for (int index = 0; index < line.size(); ++index) {
        if (line.at(index) == QLatin1Char('"')) insideQuotes = !insideQuotes;
        if (line.at(index) == QLatin1Char('#') && !insideQuotes) {
            line.truncate(index);
            break;
        }
    }
    return line.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                Qt::SkipEmptyParts);
}

QString contentForLine(QString line)
{
    bool insideQuotes = false;
    for (int index = 0; index < line.size(); ++index) {
        if (line.at(index) == QLatin1Char('"')) insideQuotes = !insideQuotes;
        if (line.at(index) == QLatin1Char('#') && !insideQuotes) {
            line.truncate(index);
            break;
        }
    }
    return line.trimmed();
}

bool looksLikeNumber(const QString &token)
{
    if (token.isEmpty()) return false;
    const QChar first = token.front();
    if (first.isDigit() || first == QLatin1Char('+') || first == QLatin1Char('-')
        || first == QLatin1Char('.')) {
        return true;
    }
    const QString lower = token.toLower();
    return lower == QStringLiteral("nan") || lower == QStringLiteral("inf")
        || lower == QStringLiteral("infinity");
}

double encodedSrgbToLinearExtended(const double value)
{
    if (value <= 0.04045) {
        return value / 12.92;
    }
    return std::pow((value + 0.055) / 1.055, 2.4);
}

double linearToEncodedSrgbExtended(const double value)
{
    if (value <= 0.0031308) {
        return value * 12.92;
    }
    return 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

std::array<double, 3> mapTransfer(
    const std::array<double, 3> &value,
    double (*transform)(double))
{
    return {
        transform(value[0]),
        transform(value[1]),
        transform(value[2])
    };
}

std::array<double, 3> sampleOneD(const LutParameters &parameters,
                                const std::array<double, 3> &input);
std::array<double, 3> sampleThreeD(const LutParameters &parameters,
                                  const std::array<double, 3> &input,
                                  LutInterpolation interpolation);

std::array<double, 3> documentToLinearRec709(
    const std::array<double, 3> &value,
    const LutDocumentTransfer transfer)
{
    return transfer == LutDocumentTransfer::EncodedSrgb
        ? mapTransfer(value, encodedSrgbToLinearExtended)
        : value;
}

std::array<double, 3> linearRec709ToDocument(
    const std::array<double, 3> &value,
    const LutDocumentTransfer transfer)
{
    return transfer == LutDocumentTransfer::EncodedSrgb
        ? mapTransfer(value, linearToEncodedSrgbExtended)
        : value;
}

std::array<double, 3> blendInDocumentSpace(
    const std::array<double, 3> &input,
    const std::array<double, 3> &mapped,
    const double strengthPercent)
{
    const double strength = strengthPercent / 100.0;
    return {
        input[0] + (mapped[0] - input[0]) * strength,
        input[1] + (mapped[1] - input[1]) * strength,
        input[2] + (mapped[2] - input[2]) * strength
    };
}

LutOperatorProfile suggestedOperatorProfile(const QString &sourceName,
                                            const QString &title)
{
    QString identity = sourceName + QLatin1Char(' ') + title;
    identity = identity.toLower();
    identity.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                     QStringLiteral(" "));
    const QString compact = QString(identity).remove(QLatin1Char(' '));
    if (compact.contains(QStringLiteral("tonymcmapface"))) {
        return LutOperatorProfile::TonyMcMapface;
    }
    if (compact.contains(QStringLiteral("agxbase"))
        && compact.contains(QStringLiteral("srgb"))) {
        return LutOperatorProfile::AgXBaseSrgb;
    }
    return LutOperatorProfile::Generic;
}

std::array<double, 3> evaluateTonyMcMapface(
    const LutParameters &parameters,
    const std::array<double, 3> &input,
    const LutDocumentTransfer documentTransfer)
{
    const std::array<double, 3> linear = documentToLinearRec709(input, documentTransfer);
    std::array<double, 3> encoded {};
    for (int channel = 0; channel < 3; ++channel) {
        const double value = linear[static_cast<std::size_t>(channel)];
        const double denominator = value + 1.0;
        encoded[static_cast<std::size_t>(channel)] =
            std::abs(denominator) <= std::numeric_limits<double>::epsilon()
                ? 0.0 : value / denominator;
    }
    const std::array<double, 3> shaped = sampleOneD(parameters, encoded);
    const std::array<double, 3> linearOutput = sampleThreeD(
        parameters, shaped, LutInterpolation::Tetrahedral);
    return linearRec709ToDocument(linearOutput, documentTransfer);
}

std::array<double, 3> evaluateAgXBaseSrgb(
    const LutParameters &parameters,
    const std::array<double, 3> &input,
    const LutDocumentTransfer documentTransfer)
{
    // This is the exact surrounding transform supplied with the LUT used by
    // the reference renderer: linear Rec.709 -> linear FilmLight E-Gamut,
    // log2 allocation over [-12.47393, 12.5260688117], tetrahedral sampling,
    // then a 2.4 power decode back to linear Rec.709 output values.
    constexpr double MinimumExposure = -12.47393;
    constexpr double MaximumExposure = 12.5260688117;
    constexpr double ExposureSpan = MaximumExposure - MinimumExposure;
    const std::array<double, 3> linear = documentToLinearRec709(input, documentTransfer);
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
    std::array<double, 3> allocated {};
    for (int channel = 0; channel < 3; ++channel) {
        const double value = filmLight[static_cast<std::size_t>(channel)];
        allocated[static_cast<std::size_t>(channel)] = value > 0.0
            ? std::clamp((std::log2(value) - MinimumExposure) / ExposureSpan,
                         0.0, 1.0)
            : 0.0;
    }
    const std::array<double, 3> shaped = sampleOneD(parameters, allocated);
    std::array<double, 3> linearOutput = sampleThreeD(
        parameters, shaped, LutInterpolation::Tetrahedral);
    for (double &component : linearOutput) {
        component = std::pow(std::max(0.0, component), 2.4);
    }
    return linearRec709ToDocument(linearOutput, documentTransfer);
}

std::array<double, 3> sampleOneD(const LutParameters &parameters,
                                const std::array<double, 3> &input)
{
    if (!parameters.hasShaper()) return input;
    std::array<double, 3> output {};
    const int size = parameters.shaperSize;
    for (int channel = 0; channel < 3; ++channel) {
        const double minimum = parameters.shaperDomainMin[static_cast<std::size_t>(channel)];
        const double maximum = parameters.shaperDomainMax[static_cast<std::size_t>(channel)];
        const double normalised = std::clamp(
            (input[static_cast<std::size_t>(channel)] - minimum) / (maximum - minimum),
            0.0, 1.0);
        const double coordinate = normalised * (size - 1);
        const int left = std::clamp(static_cast<int>(std::floor(coordinate)), 0, size - 1);
        const int right = std::min(size - 1, left + 1);
        const double fraction = coordinate - left;
        const double a = parameters.shaperData.at(left * 3 + channel);
        const double b = parameters.shaperData.at(right * 3 + channel);
        output[static_cast<std::size_t>(channel)] = a + (b - a) * fraction;
    }
    return output;
}

std::array<double, 3> sampleThreeD(const LutParameters &parameters,
                                  const std::array<double, 3> &input,
                                  const LutInterpolation interpolation)
{
    if (!parameters.hasCube()) return input;

    const int size = parameters.cubeSize;
    std::array<int, 3> low {};
    std::array<int, 3> high {};
    std::array<double, 3> fraction {};
    for (int channel = 0; channel < 3; ++channel) {
        const std::size_t c = static_cast<std::size_t>(channel);
        const double minimum = parameters.cubeDomainMin[c];
        const double maximum = parameters.cubeDomainMax[c];
        const double normalised = std::clamp(
            (input[c] - minimum) / (maximum - minimum), 0.0, 1.0);
        const double coordinate = normalised * (size - 1);
        low[c] = std::clamp(static_cast<int>(std::floor(coordinate)), 0, size - 1);
        high[c] = std::min(size - 1, low[c] + 1);
        fraction[c] = coordinate - low[c];
    }

    const auto entry = [&](const int red, const int green, const int blue) {
        std::array<double, 3> value {};
        const qsizetype index = (qsizetype(blue) * size * size
                                 + qsizetype(green) * size + red) * 3;
        for (int channel = 0; channel < 3; ++channel) {
            value[static_cast<std::size_t>(channel)] =
                static_cast<double>(parameters.cubeData.at(index + channel));
        }
        return value;
    };
    const std::array<double, 3> c000 = entry(low[0], low[1], low[2]);
    const std::array<double, 3> c100 = entry(high[0], low[1], low[2]);
    const std::array<double, 3> c010 = entry(low[0], high[1], low[2]);
    const std::array<double, 3> c110 = entry(high[0], high[1], low[2]);
    const std::array<double, 3> c001 = entry(low[0], low[1], high[2]);
    const std::array<double, 3> c101 = entry(high[0], low[1], high[2]);
    const std::array<double, 3> c011 = entry(low[0], high[1], high[2]);
    const std::array<double, 3> c111 = entry(high[0], high[1], high[2]);

    const auto linearCombination = [](const std::array<double, 3> &base,
                                      const double firstWeight,
                                      const std::array<double, 3> &firstFrom,
                                      const std::array<double, 3> &firstTo,
                                      const double secondWeight,
                                      const std::array<double, 3> &secondFrom,
                                      const std::array<double, 3> &secondTo,
                                      const double thirdWeight,
                                      const std::array<double, 3> &thirdFrom,
                                      const std::array<double, 3> &thirdTo) {
        std::array<double, 3> output {};
        for (int channel = 0; channel < 3; ++channel) {
            const std::size_t c = static_cast<std::size_t>(channel);
            output[c] = base[c]
                + firstWeight * (firstTo[c] - firstFrom[c])
                + secondWeight * (secondTo[c] - secondFrom[c])
                + thirdWeight * (thirdTo[c] - thirdFrom[c]);
        }
        return output;
    };

    if (interpolation == LutInterpolation::Tetrahedral) {
        const double red = fraction[0];
        const double green = fraction[1];
        const double blue = fraction[2];

        // Tie precedence is deliberately stable: R >= G >= B, R >= B > G,
        // B > R >= G, B >= G > R, G > B >= R, then G > R > B. Adjacent
        // tetrahedra share the same boundary triangle, but fixed branch order
        // prevents platform-dependent arithmetic ordering on exact ties.
        if (red >= green) {
            if (green >= blue) {
                return linearCombination(c000,
                                         red, c000, c100,
                                         green, c100, c110,
                                         blue, c110, c111);
            }
            if (red >= blue) {
                return linearCombination(c000,
                                         red, c000, c100,
                                         blue, c100, c101,
                                         green, c101, c111);
            }
            return linearCombination(c000,
                                     blue, c000, c001,
                                     red, c001, c101,
                                     green, c101, c111);
        }
        if (blue >= green) {
            return linearCombination(c000,
                                     blue, c000, c001,
                                     green, c001, c011,
                                     red, c011, c111);
        }
        if (blue >= red) {
            return linearCombination(c000,
                                     green, c000, c010,
                                     blue, c010, c011,
                                     red, c011, c111);
        }
        return linearCombination(c000,
                                 green, c000, c010,
                                 red, c010, c110,
                                 blue, c110, c111);
    }

    std::array<double, 3> output {};
    for (int channel = 0; channel < 3; ++channel) {
        const std::size_t c = static_cast<std::size_t>(channel);
        const double xr00 = c000[c] + (c100[c] - c000[c]) * fraction[0];
        const double xr10 = c010[c] + (c110[c] - c010[c]) * fraction[0];
        const double xr01 = c001[c] + (c101[c] - c001[c]) * fraction[0];
        const double xr11 = c011[c] + (c111[c] - c011[c]) * fraction[0];
        const double yg0 = xr00 + (xr10 - xr00) * fraction[1];
        const double yg1 = xr01 + (xr11 - xr01) * fraction[1];
        output[c] = yg0 + (yg1 - yg0) * fraction[2];
    }
    return output;
}

} // namespace

bool CubeLut::parse(const QByteArray &contents,
                    const QString &sourceName,
                    LutParameters *parameters,
                    QString *error)
{
    if (error) error->clear();
    if (!parameters) {
        if (error) *error = QStringLiteral("No destination was supplied for the LUT");
        return false;
    }
    if (contents.isEmpty()) {
        if (error) *error = QStringLiteral("The LUT file is empty");
        return false;
    }

    struct SeenDirective {
        bool seen = false;
        int line = 0;
    };
    enum class ParseSection {
        Header,
        ShaperData,
        CubeData,
        Complete
    };

    LutParameters parsed;
    parsed.sourceName = QFileInfo(sourceName).fileName();
    ParseSection section = ParseSection::Header;
    qsizetype shaperRowsRead = 0;
    qsizetype cubeRowsRead = 0;
    bool headerFinalised = false;

    SeenDirective titleDirective;
    SeenDirective oneDSizeDirective;
    SeenDirective threeDSizeDirective;
    SeenDirective domainMinDirective;
    SeenDirective domainMaxDirective;
    SeenDirective oneDRangeDirective;
    SeenDirective threeDRangeDirective;

    std::array<double, 3> genericDomainMin {0.0, 0.0, 0.0};
    std::array<double, 3> genericDomainMax {1.0, 1.0, 1.0};
    std::array<double, 3> oneDRangeMin {0.0, 0.0, 0.0};
    std::array<double, 3> oneDRangeMax {1.0, 1.0, 1.0};
    std::array<double, 3> threeDRangeMin {0.0, 0.0, 0.0};
    std::array<double, 3> threeDRangeMax {1.0, 1.0, 1.0};

    const auto setError = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const auto failAt = [&](const int line, const QString &message) {
        return setError(QStringLiteral("%1 at line %2").arg(message).arg(line));
    };
    const auto markUnique = [&](SeenDirective *directive,
                                const QString &name,
                                const int line) {
        if (directive->seen) {
            return failAt(line,
                          QStringLiteral("Duplicate %1 (first declared at line %2)")
                              .arg(name).arg(directive->line));
        }
        directive->seen = true;
        directive->line = line;
        return true;
    };
    const auto validateDomain = [&](const std::array<double, 3> &minimum,
                                    const std::array<double, 3> &maximum,
                                    const QString &name,
                                    const int line) {
        for (int channel = 0; channel < 3; ++channel) {
            const double low = minimum[static_cast<std::size_t>(channel)];
            const double high = maximum[static_cast<std::size_t>(channel)];
            const double span = high - low;
            if (!std::isfinite(low) || !std::isfinite(high)
                || !std::isfinite(span) || span <= 0.0) {
                return failAt(line,
                              QStringLiteral("%1 has a non-positive or unrepresentable range")
                                  .arg(name));
            }
        }
        return true;
    };
    const auto finaliseHeader = [&]() {
        if (headerFinalised) return true;
        if (parsed.shaperSize == 0 && parsed.cubeSize == 0) {
            return setError(QStringLiteral(
                "The file does not declare LUT_1D_SIZE or LUT_3D_SIZE"));
        }
        if (oneDRangeDirective.seen && parsed.shaperSize == 0) {
            return failAt(oneDRangeDirective.line,
                          QStringLiteral("LUT_1D_INPUT_RANGE was declared without LUT_1D_SIZE"));
        }
        if (threeDRangeDirective.seen && parsed.cubeSize == 0) {
            return failAt(threeDRangeDirective.line,
                          QStringLiteral("LUT_3D_INPUT_RANGE was declared without LUT_3D_SIZE"));
        }

        if (domainMinDirective.seen || domainMaxDirective.seen) {
            const int line = domainMinDirective.seen
                ? domainMinDirective.line : domainMaxDirective.line;
            if (!validateDomain(genericDomainMin, genericDomainMax,
                                QStringLiteral("DOMAIN_MIN / DOMAIN_MAX"), line)) {
                return false;
            }
            if (parsed.shaperSize > 0) {
                parsed.shaperDomainMin = genericDomainMin;
                parsed.shaperDomainMax = genericDomainMax;
                parsed.shaperDomainSource = LutDomainSource::DomainDirective;
            }
            if (parsed.cubeSize > 0) {
                parsed.cubeDomainMin = genericDomainMin;
                parsed.cubeDomainMax = genericDomainMax;
                parsed.cubeDomainSource = LutDomainSource::DomainDirective;
            }
        }
        if (oneDRangeDirective.seen) {
            parsed.shaperDomainMin = oneDRangeMin;
            parsed.shaperDomainMax = oneDRangeMax;
            parsed.shaperDomainSource = LutDomainSource::InputRangeDirective;
        }
        if (threeDRangeDirective.seen) {
            parsed.cubeDomainMin = threeDRangeMin;
            parsed.cubeDomainMax = threeDRangeMax;
            parsed.cubeDomainSource = LutDomainSource::InputRangeDirective;
        }

        parsed.shaperData.reserve(qsizetype(parsed.shaperSize) * 3);
        const qsizetype cubeFloatCount = parsed.cubeSize > 0
            ? qsizetype(parsed.cubeSize) * parsed.cubeSize * parsed.cubeSize * 3 : 0;
        parsed.cubeData.reserve(cubeFloatCount);
        section = parsed.shaperSize > 0 ? ParseSection::ShaperData
                                       : ParseSection::CubeData;
        headerFinalised = true;
        return true;
    };

    const QString text = QString::fromUtf8(contents);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const int lineNumber = lineIndex + 1;
        QString rawLine = lines.at(lineIndex);
        if (lineIndex == 0 && !rawLine.isEmpty() && rawLine.front().unicode() == 0xfeff) {
            rawLine.remove(0, 1);
        }
        const QString line = contentForLine(rawLine);
        if (line.isEmpty()) continue;
        const QStringList tokens = tokensForLine(line);
        if (tokens.isEmpty()) continue;

        bool dataRow = tokens.size() == 3;
        std::array<double, 3> decodedRow {};
        if (dataRow) {
            for (int channel = 0; channel < 3; ++channel) {
                if (!parseFinite(tokens.at(channel),
                                 &decodedRow[static_cast<std::size_t>(channel)])) {
                    dataRow = false;
                    break;
                }
            }
        }
        const bool numericContent = dataRow || looksLikeNumber(tokens.constFirst());
        if (numericContent) {
            if (!dataRow) {
                return failAt(lineNumber, QStringLiteral("Invalid LUT data row"));
            }
            if (!finaliseHeader()) return false;
            if (section == ParseSection::Complete) {
                return failAt(lineNumber, QStringLiteral("Unexpected extra LUT data row"));
            }

            std::array<float, 3> storedRow {};
            for (int channel = 0; channel < 3; ++channel) {
                const double component = decodedRow[static_cast<std::size_t>(channel)];
                if (component < -std::numeric_limits<float>::max()
                    || component > std::numeric_limits<float>::max()) {
                    return failAt(lineNumber,
                                  QStringLiteral("LUT data value is outside the supported finite float range"));
                }
                const float stored = static_cast<float>(component);
                if (!std::isfinite(stored)) {
                    return failAt(lineNumber,
                                  QStringLiteral("LUT data value is outside the supported finite float range"));
                }
                storedRow[static_cast<std::size_t>(channel)] = stored;
            }

            if (section == ParseSection::ShaperData) {
                parsed.shaperData << storedRow[0] << storedRow[1] << storedRow[2];
                ++shaperRowsRead;
                if (shaperRowsRead == parsed.shaperSize) {
                    section = parsed.cubeSize > 0 ? ParseSection::CubeData
                                                 : ParseSection::Complete;
                }
            } else if (section == ParseSection::CubeData) {
                parsed.cubeData << storedRow[0] << storedRow[1] << storedRow[2];
                ++cubeRowsRead;
                const qsizetype expectedCubeRows = qsizetype(parsed.cubeSize)
                    * parsed.cubeSize * parsed.cubeSize;
                if (cubeRowsRead == expectedCubeRows) section = ParseSection::Complete;
            }
            continue;
        }

        if (section != ParseSection::Header) {
            return failAt(lineNumber,
                          QStringLiteral("LUT directive or text appears after table data began"));
        }

        const QString keyword = tokens.constFirst().toUpper();
        if (keyword == QStringLiteral("TITLE")) {
            if (!markUnique(&titleDirective, QStringLiteral("TITLE"), lineNumber)) return false;
            QString title = line.mid(tokens.constFirst().size()).trimmed();
            if (title.isEmpty()) return failAt(lineNumber, QStringLiteral("Invalid TITLE"));
            if (title.startsWith(QLatin1Char('"'))) {
                if (title.size() < 2 || !title.endsWith(QLatin1Char('"'))) {
                    return failAt(lineNumber, QStringLiteral("Unterminated quoted TITLE"));
                }
                title = title.mid(1, title.size() - 2);
            }
            parsed.title = title;
            continue;
        }
        if (keyword == QStringLiteral("LUT_1D_SIZE")) {
            if (!markUnique(&oneDSizeDirective, keyword, lineNumber)) return false;
            if (tokens.size() != 2 || !parseInteger(tokens.at(1), &parsed.shaperSize)
                || parsed.shaperSize < 2 || parsed.shaperSize > MaximumOneDSize) {
                return failAt(lineNumber,
                              QStringLiteral("Invalid LUT_1D_SIZE (supported range is 2 to 65536)"));
            }
            continue;
        }
        if (keyword == QStringLiteral("LUT_3D_SIZE")) {
            if (!markUnique(&threeDSizeDirective, keyword, lineNumber)) return false;
            if (tokens.size() != 2 || !parseInteger(tokens.at(1), &parsed.cubeSize)
                || parsed.cubeSize < 2 || parsed.cubeSize > MaximumThreeDSize) {
                return failAt(lineNumber,
                              QStringLiteral("Invalid LUT_3D_SIZE (supported range is 2 to 65)"));
            }
            continue;
        }
        if (keyword == QStringLiteral("DOMAIN_MIN")
            || keyword == QStringLiteral("DOMAIN_MAX")) {
            SeenDirective *directive = keyword == QStringLiteral("DOMAIN_MIN")
                ? &domainMinDirective : &domainMaxDirective;
            if (!markUnique(directive, keyword, lineNumber)) return false;
            if (oneDRangeDirective.seen || threeDRangeDirective.seen) {
                const int specificLine = oneDRangeDirective.seen
                    ? oneDRangeDirective.line : threeDRangeDirective.line;
                return failAt(lineNumber,
                              QStringLiteral("%1 conflicts with a LUT-specific input range declared at line %2")
                                  .arg(keyword).arg(specificLine));
            }
            if (tokens.size() != 4) {
                return failAt(lineNumber, QStringLiteral("Invalid %1").arg(keyword));
            }
            auto &domain = keyword == QStringLiteral("DOMAIN_MIN")
                ? genericDomainMin : genericDomainMax;
            for (int channel = 0; channel < 3; ++channel) {
                if (!parseFinite(tokens.at(channel + 1),
                                 &domain[static_cast<std::size_t>(channel)])) {
                    return failAt(lineNumber, QStringLiteral("Invalid %1").arg(keyword));
                }
            }
            continue;
        }
        if (keyword == QStringLiteral("LUT_1D_INPUT_RANGE")
            || keyword == QStringLiteral("LUT_3D_INPUT_RANGE")) {
            SeenDirective *directive = keyword == QStringLiteral("LUT_1D_INPUT_RANGE")
                ? &oneDRangeDirective : &threeDRangeDirective;
            if (!markUnique(directive, keyword, lineNumber)) return false;
            if (domainMinDirective.seen || domainMaxDirective.seen) {
                const int genericLine = domainMinDirective.seen
                    ? domainMinDirective.line : domainMaxDirective.line;
                return failAt(lineNumber,
                              QStringLiteral("%1 conflicts with DOMAIN_MIN / DOMAIN_MAX declared at line %2")
                                  .arg(keyword).arg(genericLine));
            }
            if (tokens.size() != 3) {
                return failAt(lineNumber, QStringLiteral("Invalid %1").arg(keyword));
            }
            double minimum = 0.0;
            double maximum = 1.0;
            if (!parseFinite(tokens.at(1), &minimum)
                || !parseFinite(tokens.at(2), &maximum)) {
                return failAt(lineNumber, QStringLiteral("Invalid %1").arg(keyword));
            }
            const std::array<double, 3> mins {minimum, minimum, minimum};
            const std::array<double, 3> maxs {maximum, maximum, maximum};
            if (!validateDomain(mins, maxs, keyword, lineNumber)) return false;
            if (keyword == QStringLiteral("LUT_1D_INPUT_RANGE")) {
                oneDRangeMin = mins;
                oneDRangeMax = maxs;
            } else {
                threeDRangeMin = mins;
                threeDRangeMax = maxs;
            }
            continue;
        }
        if (keyword == QStringLiteral("LUT_IN_VIDEO_RANGE")
            || keyword == QStringLiteral("LUT_OUT_VIDEO_RANGE")) {
            if (tokens.size() != 1) {
                return failAt(lineNumber, QStringLiteral("Invalid %1").arg(keyword));
            }
            return failAt(lineNumber,
                          QStringLiteral("Unsupported %1: video-range compensation is not implemented, and importing it as data range would be incorrect")
                              .arg(keyword));
        }

        return failAt(lineNumber,
                      QStringLiteral("Unsupported LUT directive '%1'").arg(tokens.constFirst()));
    }

    if (!finaliseHeader()) return false;
    if (section == ParseSection::ShaperData) {
        return setError(QStringLiteral("LUT_1D_SIZE declares %1 data rows but only %2 were provided")
                            .arg(parsed.shaperSize).arg(shaperRowsRead));
    }
    if (section == ParseSection::CubeData) {
        const qsizetype expectedCubeRows = qsizetype(parsed.cubeSize)
            * parsed.cubeSize * parsed.cubeSize;
        return setError(QStringLiteral("LUT_3D_SIZE declares %1 data rows but only %2 were provided")
                            .arg(expectedCubeRows).arg(cubeRowsRead));
    }

    parsed.operatorProfile = suggestedOperatorProfile(parsed.sourceName, parsed.title);
    parsed.normalise();
    if (!parsed.hasData()) {
        if (error) *error = QStringLiteral("The LUT table could not be validated");
        return false;
    }
    *parameters = parsed;
    return true;
}

bool CubeLut::loadFile(const QString &filePath,
                       LutParameters *parameters,
                       QString *error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Could not open %1: %2")
                                .arg(QFileInfo(filePath).fileName(), file.errorString());
        return false;
    }
    constexpr qint64 MaximumCubeFileBytes = 64ll * 1024 * 1024;
    if (file.size() <= 0 || file.size() > MaximumCubeFileBytes) {
        if (error) *error = QStringLiteral("The LUT file is empty or exceeds the 64 MiB safety limit");
        return false;
    }
    return parse(file.readAll(), filePath, parameters, error);
}

LutDocumentTransfer CubeLut::documentTransferFor(const QColorSpace &colourSpace)
{
    if (!colourSpace.isValid()) {
        // Untagged images have historically been treated as ordinary encoded
        // sRGB component data throughout VFX Photo Lab. Keep that compatibility
        // contract explicit until profile assignment arrives in 0.11.0.
        return LutDocumentTransfer::EncodedSrgb;
    }

    // Inspect the semantic primaries/transfer pair rather than comparing only
    // against Qt's built-in named instances. Image readers often reconstruct an
    // embedded ICC sRGB profile as an equivalent custom QColorSpace object.
    if (colourSpace.primaries() == QColorSpace::Primaries::SRgb) {
        if (colourSpace.transferFunction()
            == QColorSpace::TransferFunction::Linear) {
            return LutDocumentTransfer::LinearSrgb;
        }
        if (colourSpace.transferFunction()
            == QColorSpace::TransferFunction::SRgb) {
            return LutDocumentTransfer::EncodedSrgb;
        }
    }
    return LutDocumentTransfer::UnsupportedProfile;
}

bool CubeLut::requiresCpuEvaluation(const LutParameters &parameters,
                                    const QColorSpace &colourSpace)
{
    Q_UNUSED(colourSpace)
    return !gpuFallbackReason(parameters).isEmpty();
}

QString CubeLut::gpuFallbackReason(const LutParameters &inputParameters)
{
    LutParameters parameters = inputParameters;
    parameters.normalise();
    if (!parameters.hasData()) return {};

    if (!parameters.gpuTableHalfFloatCompatible) {
        return QStringLiteral(
            "One or more LUT table samples are outside finite RGBA16Float range; the authoritative CPU evaluator will be used.");
    }
    if (!parameters.gpuHalfFloatCompatible) {
        return QStringLiteral(
            "One or more LUT input-domain values cannot be represented safely by the native f32 path; the authoritative CPU evaluator will be used.");
    }

    const int width = std::max(parameters.hasCube()
                                   ? parameters.cubeSize * parameters.cubeSize : 0,
                               parameters.hasShaper()
                                   ? parameters.shaperSize : 0);
    const int height = (parameters.hasCube() ? parameters.cubeSize : 0)
        + (parameters.hasShaper() ? 1 : 0);
    if (width <= 0 || height <= 0) {
        return QStringLiteral(
            "The LUT does not contain a valid native lookup texture layout; the authoritative CPU evaluator will be used.");
    }
    if (width > 8192 || height > 8192) {
        return QStringLiteral(
            "The packed LUT lookup texture exceeds the conservative 8192-pixel native dimension limit; the authoritative CPU evaluator will be used.");
    }
    return {};
}

QString CubeLut::processingWarning(const LutParameters &parameters,
                                   const QColorSpace &colourSpace)
{
    if (!parameters.hasData()) return {};

    QStringList warnings;
    const LutDocumentTransfer transfer = documentTransferFor(colourSpace);
    if (parameters.operatorProfile == LutOperatorProfile::Generic) {
        if (parameters.processingMode != LutProcessingMode::RawComponents
            && transfer == LutDocumentTransfer::UnsupportedProfile) {
            warnings << QStringLiteral(
                "This document uses an ICC profile outside the current sRGB / linear-sRGB contract. "
                "VFX Photo Lab will preserve and sample its stored components without a hidden gamut "
                "conversion. Full arbitrary working-space conversion is scheduled for 0.11.0.");
        }
        return warnings.join(QStringLiteral("\n\n"));
    }

    if (!parameters.hasCube()) {
        warnings << QStringLiteral(
            "The selected operator profile requires a 3D LUT table. Load its matching .cube file or switch to Generic .cube.");
    }
    if (transfer == LutDocumentTransfer::UnsupportedProfile) {
        warnings << QStringLiteral(
            "This display transform requires linear Rec.709 / sRGB primaries. The document profile is outside that contract, so stored components are used without a guessed gamut conversion until 0.11.0.");
    }
    if (parameters.operatorProfile == LutOperatorProfile::TonyMcMapface
        && parameters.hasCube() && parameters.cubeSize != 48) {
        warnings << QStringLiteral(
            "The reference Tony McMapface table is 48³; the loaded table is %1³. The pipeline will run, but it may not reproduce the reference transform.")
                        .arg(parameters.cubeSize);
    }
    if (parameters.operatorProfile == LutOperatorProfile::AgXBaseSrgb
        && parameters.hasCube() && parameters.cubeSize != 57) {
        warnings << QStringLiteral(
            "The reference AgX Base sRGB table is 57³; the loaded table is %1³. The pipeline will run, but it may not reproduce the reference transform.")
                        .arg(parameters.cubeSize);
    }
    return warnings.join(QStringLiteral("\n\n"));
}

std::array<double, 3> CubeLut::evaluate(
    const LutParameters &parameters,
    const std::array<double, 3> &input,
    const LutDocumentTransfer documentTransfer)
{
    // AdjustmentData normalises LUT payloads at load/update boundaries. Do not
    // copy or rescan the embedded table for every pixel; a 65-point cube holds
    // more than 800,000 floats and must remain a read-only shared payload here.
    if (!parameters.hasData() || parameters.strength <= 0.0) return input;

    if (parameters.operatorProfile == LutOperatorProfile::TonyMcMapface) {
        return blendInDocumentSpace(
            input, evaluateTonyMcMapface(parameters, input, documentTransfer),
            parameters.strength);
    }
    if (parameters.operatorProfile == LutOperatorProfile::AgXBaseSrgb) {
        return blendInDocumentSpace(
            input, evaluateAgXBaseSrgb(parameters, input, documentTransfer),
            parameters.strength);
    }

    std::array<double, 3> tableInput = input;
    const bool encodedTableOnLinearDocument =
        parameters.processingMode == LutProcessingMode::EncodedDocument
        && documentTransfer == LutDocumentTransfer::LinearSrgb;
    const bool linearTableOnEncodedDocument =
        parameters.processingMode == LutProcessingMode::LinearSrgb
        && documentTransfer == LutDocumentTransfer::EncodedSrgb;

    if (encodedTableOnLinearDocument) {
        tableInput = mapTransfer(input, linearToEncodedSrgbExtended);
    } else if (linearTableOnEncodedDocument) {
        tableInput = mapTransfer(input, encodedSrgbToLinearExtended);
    }

    const std::array<double, 3> shaped = sampleOneD(parameters, tableInput);
    std::array<double, 3> mapped = sampleThreeD(
        parameters, shaped, parameters.interpolation);

    if (encodedTableOnLinearDocument) {
        mapped = mapTransfer(mapped, encodedSrgbToLinearExtended);
    } else if (linearTableOnEncodedDocument) {
        mapped = mapTransfer(mapped, linearToEncodedSrgbExtended);
    }

    return blendInDocumentSpace(input, mapped, parameters.strength);
}

LutGpuTextureData CubeLut::buildGpuTextureData(
    const LutParameters &inputParameters,
    QString *error)
{
    if (error) error->clear();
    LutParameters parameters = inputParameters;
    parameters.normalise();
    if (!parameters.hasData()) return {};
    const QString fallbackReason = gpuFallbackReason(parameters);
    if (!fallbackReason.isEmpty()) {
        if (error) *error = fallbackReason;
        return {};
    }

    const int cubeWidth = parameters.hasCube()
        ? parameters.cubeSize * parameters.cubeSize : 0;
    const int width = std::max(cubeWidth,
                               parameters.hasShaper() ? parameters.shaperSize : 0);
    const int row = parameters.hasCube() ? parameters.cubeSize : 0;
    const int height = row + (parameters.hasShaper() ? 1 : 0);
    LutGpuTextureData result;
    result.size = QSize(width, height);
    result.shaperRow = row;
    result.rgba16f.resize(qsizetype(width) * height * 4);
    std::fill(result.rgba16f.begin(), result.rgba16f.end(), qfloat16(0.0f));
    for (qsizetype pixel = 0; pixel < qsizetype(width) * height; ++pixel) {
        result.rgba16f[pixel * 4 + 3] = qfloat16(1.0f);
    }

    if (parameters.hasCube()) {
        for (int blue = 0; blue < parameters.cubeSize; ++blue) {
            for (int green = 0; green < parameters.cubeSize; ++green) {
                for (int red = 0; red < parameters.cubeSize; ++red) {
                    const qsizetype tableIndex =
                        (qsizetype(blue) * parameters.cubeSize * parameters.cubeSize
                         + qsizetype(green) * parameters.cubeSize + red) * 3;
                    const int x = red + blue * parameters.cubeSize;
                    const qsizetype textureIndex =
                        (qsizetype(green) * width + x) * 4;
                    for (int channel = 0; channel < 3; ++channel) {
                        result.rgba16f[textureIndex + channel] = qfloat16(
                            parameters.cubeData.at(tableIndex + channel));
                    }
                }
            }
        }
    }
    if (parameters.hasShaper()) {
        for (int index = 0; index < parameters.shaperSize; ++index) {
            const qsizetype textureIndex =
                (qsizetype(row) * width + index) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                result.rgba16f[textureIndex + channel] = qfloat16(
                    parameters.shaperData.at(index * 3 + channel));
            }
        }
    }
    return result;
}

} // namespace vfx
