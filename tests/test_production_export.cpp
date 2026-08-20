#include "ProductionExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QColorSpace>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

using namespace vfx;

namespace {

DocumentColourState colourState()
{
    DocumentColourState state;
    state.workingSpace = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    state.output.profile = state.workingSpace;
    return state;
}

ExportProfileData pngProfile(const QString &nameTemplate)
{
    ExportProfileData data;
    data.formatSuffix = QStringLiteral("png");
    data.bitDepth = ImageExportBitDepth::Eight;
    data.dither = ImageExportDither::BlueNoise64;
    data.alphaMode = ImageExportAlphaMode::PreserveWhenSupported;
    data.convertToOutputProfile = true;
    data.output.profile = ColourSpaceDescriptor::fromQColorSpace(
        QColorSpace(QColorSpace::SRgb));
    data.quality = 95;
    data.namingTemplate = nameTemplate;
    return data;
}

ProductionExportPlan planFor(const QString &directory)
{
    ProductionExportPlan plan;
    plan.outputDirectory = directory;
    plan.documentName = QStringLiteral("Portrait.vfxphoto");
    plan.documentSize = QSize(4000, 3000);
    plan.workingSpaceName = QStringLiteral("sRGB");
    plan.timestampUtc = QDateTime::fromString(
        QStringLiteral("2026-08-04T09:00:00Z"), Qt::ISODate);
    return plan;
}

ProductionExportOutput outputFor(const QString &id,
                                 const QString &profileName,
                                 const QString &nameTemplate)
{
    ProductionExportOutput output;
    output.id = id;
    output.profileId = QStringLiteral("profile-") + id;
    output.profileName = profileName;
    output.profile = pngProfile(nameTemplate);
    output.namingTemplate = nameTemplate;
    output.resize.width = 4000;
    output.resize.height = 3000;
    output.resize.longEdge = 4000;
    return output;
}

} // namespace

class ProductionExportTests final : public QObject {
    Q_OBJECT

private slots:
    void resizeModesAreDeterministic();
    void resolvesMultipleOutputsAndTokens();
    void duplicatePathsAreRejectedWithoutAutoRename();
    void autoRenameIsDeterministicAndNonDestructive();
    void skipExistingMarksOnlyTheCollision();
    void askBeforeStartMarksConfirmedExistingFile();
    void disabledInvalidOutputDoesNotBlockEnabledOutput();
    void stableOutputIdentifiersAreRequired();
    void invalidTimestampResamplingAndCollisionPolicyAreRejected();
    void unsafeSurfaceIsRejectedWithoutPartialResults();
    void outputCountIsBounded();
    void executionAutoRenameAvoidsReservedPaths();
    void invalidProfileDoesNotPartiallyResolve();
    void resolvedPayloadValidationAcceptsAutoRenameAndRejectsTampering();
    void resolvedPayloadValidationRejectsDuplicateDestinations();
};

void ProductionExportTests::resizeModesAreDeterministic()
{
    ProductionExportResize resize;
    QCOMPARE(resize.resolvedSize(QSize(4000, 3000)), QSize(4000, 3000));

    resize.mode = ProductionExportResizeMode::ExactPixels;
    resize.width = 1920;
    resize.height = 1080;
    resize.preserveAspect = true;
    QCOMPARE(resize.resolvedSize(QSize(4000, 3000)), QSize(1440, 1080));

    resize.preserveAspect = false;
    QCOMPARE(resize.resolvedSize(QSize(4000, 3000)), QSize(1920, 1080));

    resize.mode = ProductionExportResizeMode::LongEdge;
    resize.longEdge = 2000;
    QCOMPARE(resize.resolvedSize(QSize(4000, 3000)), QSize(2000, 1500));

    resize.mode = ProductionExportResizeMode::Percentage;
    resize.percentage = 25.0;
    QCOMPARE(resize.resolvedSize(QSize(4000, 3000)), QSize(1000, 750));
}

void ProductionExportTests::resolvesMultipleOutputsAndTokens()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProductionExportPlan plan = planFor(directory.path());
    ProductionExportOutput master = outputFor(
        QStringLiteral("master"), QStringLiteral("Master"),
        QStringLiteral("{document}-{profile}-{width}x{height}"));
    ProductionExportOutput web = outputFor(
        QStringLiteral("web"), QStringLiteral("Web"),
        QStringLiteral("{document}-{profile}-{bit_depth}"));
    web.resize.mode = ProductionExportResizeMode::LongEdge;
    web.resize.longEdge = 1600;
    plan.outputs = {master, web};

    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY2(resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error),
             qPrintable(error));
    QCOMPARE(resolved.size(), 2);
    QCOMPARE(resolved.at(0).outputSize, QSize(4000, 3000));
    QCOMPARE(resolved.at(1).outputSize, QSize(1600, 1200));
    QCOMPARE(QFileInfo(resolved.at(0).request.filePath).fileName(),
             QStringLiteral("Portrait-Master-4000x3000.png"));
    QCOMPARE(QFileInfo(resolved.at(1).request.filePath).fileName(),
             QStringLiteral("Portrait-Web-8bit.png"));
}

void ProductionExportTests::duplicatePathsAreRejectedWithoutAutoRename()
{
    QTemporaryDir directory;
    ProductionExportPlan plan = planFor(directory.path());
    plan.outputs = {
        outputFor(QStringLiteral("one"), QStringLiteral("One"), QStringLiteral("same")),
        outputFor(QStringLiteral("two"), QStringLiteral("Two"), QStringLiteral("same"))
    };
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("same file"), Qt::CaseInsensitive));
}

void ProductionExportTests::autoRenameIsDeterministicAndNonDestructive()
{
    QTemporaryDir directory;
    QFile existing(QDir(directory.path()).filePath(QStringLiteral("same.png")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("old");
    existing.close();

    ProductionExportPlan plan = planFor(directory.path());
    plan.collisionPolicy = ProductionExportCollisionPolicy::AutoRename;
    plan.outputs = {
        outputFor(QStringLiteral("one"), QStringLiteral("One"), QStringLiteral("same")),
        outputFor(QStringLiteral("two"), QStringLiteral("Two"), QStringLiteral("same"))
    };
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY2(resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error), qPrintable(error));
    QCOMPARE(QFileInfo(resolved.at(0).request.filePath).fileName(),
             QStringLiteral("same-2.png"));
    QCOMPARE(QFileInfo(resolved.at(1).request.filePath).fileName(),
             QStringLiteral("same-3.png"));
    QVERIFY(!resolved.at(0).existedAtPreflight);
    QVERIFY(!resolved.at(1).existedAtPreflight);
    QVERIFY(QFileInfo::exists(existing.fileName()));
}

void ProductionExportTests::skipExistingMarksOnlyTheCollision()
{
    QTemporaryDir directory;
    QFile existing(QDir(directory.path()).filePath(QStringLiteral("one.png")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.close();
    ProductionExportPlan plan = planFor(directory.path());
    plan.collisionPolicy = ProductionExportCollisionPolicy::SkipExisting;
    plan.outputs = {
        outputFor(QStringLiteral("one"), QStringLiteral("One"), QStringLiteral("one")),
        outputFor(QStringLiteral("two"), QStringLiteral("Two"), QStringLiteral("two"))
    };
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY2(resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error), qPrintable(error));
    QVERIFY(resolved.at(0).skipExisting);
    QVERIFY(!resolved.at(1).skipExisting);
}


void ProductionExportTests::askBeforeStartMarksConfirmedExistingFile()
{
    QTemporaryDir directory;
    QFile existing(QDir(directory.path()).filePath(QStringLiteral("confirmed.png")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("old");
    existing.close();

    ProductionExportPlan plan = planFor(directory.path());
    plan.collisionPolicy = ProductionExportCollisionPolicy::AskBeforeStart;
    plan.outputs = {outputFor(QStringLiteral("confirmed"),
                              QStringLiteral("Confirmed"),
                              QStringLiteral("confirmed"))};
    QVector<ResolvedProductionExportOutput> resolved;
    QStringList warnings;
    QString error;
    QVERIFY2(resolveProductionExportPlan(plan, colourState(), &resolved,
                                         &warnings, &error), qPrintable(error));
    QCOMPARE(resolved.size(), 1);
    QVERIFY(resolved.at(0).existedAtPreflight);
    QVERIFY(!resolved.at(0).skipExisting);
    QCOMPARE(warnings.size(), 1);
    QVERIFY(warnings.constFirst().contains(QStringLiteral("confirmed.png")));
}

void ProductionExportTests::disabledInvalidOutputDoesNotBlockEnabledOutput()
{
    QTemporaryDir directory;
    ProductionExportPlan plan = planFor(directory.path());
    ProductionExportOutput enabled = outputFor(
        QStringLiteral("enabled"), QStringLiteral("Enabled"),
        QStringLiteral("enabled"));
    ProductionExportOutput disabled = outputFor(
        QStringLiteral("disabled"), QStringLiteral("Disabled"),
        QStringLiteral("disabled"));
    disabled.enabled = false;
    disabled.profileId.clear();
    disabled.profileName.clear();
    disabled.profile.formatSuffix = QStringLiteral("not-a-format");
    disabled.resize.method = static_cast<ImageResampleMethod>(255);
    plan.outputs = {enabled, disabled};

    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY2(resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error), qPrintable(error));
    QCOMPARE(resolved.size(), 1);
    QCOMPARE(resolved.constFirst().id, QStringLiteral("enabled"));
}

void ProductionExportTests::stableOutputIdentifiersAreRequired()
{
    QTemporaryDir directory;
    ProductionExportPlan plan = planFor(directory.path());
    ProductionExportOutput first = outputFor(
        QStringLiteral("same-id"), QStringLiteral("One"), QStringLiteral("one"));
    ProductionExportOutput second = outputFor(
        QStringLiteral("same-id"), QStringLiteral("Two"), QStringLiteral("two"));
    plan.outputs = {first, second};
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("unique stable identifier"),
                           Qt::CaseInsensitive));

    plan.outputs = {first};
    plan.outputs[0].id.clear();
    error.clear();
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());

    plan.outputs = {first};
    plan.outputs[0].id = QStringLiteral(" leading-space");
    error.clear();
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());

    plan.outputs = {first};
    plan.outputs[0].profileId = QStringLiteral(" Invalid-Profile");
    error.clear();
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("profile identifier"),
                           Qt::CaseInsensitive));

    plan.outputs = {first};
    plan.outputs[0].profileName = QStringLiteral(" Padded Name ");
    error.clear();
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
}

void ProductionExportTests::invalidTimestampResamplingAndCollisionPolicyAreRejected()
{
    QTemporaryDir directory;
    ProductionExportPlan plan = planFor(directory.path());
    plan.outputs = {outputFor(QStringLiteral("one"), QStringLiteral("One"),
                              QStringLiteral("one"))};
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;

    plan.timestampUtc = QDateTime();
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("timestamp"), Qt::CaseInsensitive));

    plan = planFor(directory.path());
    plan.outputs = {outputFor(QStringLiteral("one"), QStringLiteral("One"),
                              QStringLiteral("one"))};
    plan.outputs[0].resize.method = static_cast<ImageResampleMethod>(255);
    error.clear();
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("resampling"), Qt::CaseInsensitive));

    plan = planFor(directory.path());
    plan.outputs = {outputFor(QStringLiteral("one"), QStringLiteral("One"),
                              QStringLiteral("one"))};
    plan.collisionPolicy = static_cast<ProductionExportCollisionPolicy>(255);
    error.clear();
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("collision policy"), Qt::CaseInsensitive));
}

void ProductionExportTests::unsafeSurfaceIsRejectedWithoutPartialResults()
{
    QTemporaryDir directory;
    ProductionExportPlan plan = planFor(directory.path());
    ProductionExportOutput normal = outputFor(
        QStringLiteral("normal"), QStringLiteral("Normal"), QStringLiteral("normal"));
    ProductionExportOutput huge = outputFor(
        QStringLiteral("huge"), QStringLiteral("Huge"), QStringLiteral("huge"));
    huge.resize.mode = ProductionExportResizeMode::ExactPixels;
    huge.resize.width = 32768;
    huge.resize.height = 32768;
    huge.resize.preserveAspect = false;
    plan.outputs = {normal, huge};
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("too large"), Qt::CaseInsensitive));
}


void ProductionExportTests::outputCountIsBounded()
{
    QTemporaryDir directory;
    ProductionExportPlan plan = planFor(directory.path());
    for (int index = 0; index < 33; ++index) {
        ProductionExportOutput output = outputFor(
            QStringLiteral("output-%1").arg(index),
            QStringLiteral("Output %1").arg(index),
            QStringLiteral("output-%1").arg(index));
        if (index == 32) output.enabled = false;
        plan.outputs.push_back(std::move(output));
    }
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
    QVERIFY(error.contains(QStringLiteral("at most 32"), Qt::CaseInsensitive));
}

void ProductionExportTests::executionAutoRenameAvoidsReservedPaths()
{
    QTemporaryDir directory;
    const QString requested = QDir(directory.path()).filePath(
        QStringLiteral("output.png"));
    QFile file(requested);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("existing");
    file.close();
    const QString reserved = QDir(directory.path()).filePath(
        QStringLiteral("output-2.png"));
    const QString unique = uniqueProductionExportOutputPath(
        requested, {requested, reserved});
    QCOMPARE(QFileInfo(unique).fileName(), QStringLiteral("output-3.png"));
    QVERIFY(!QFileInfo::exists(unique));
}

void ProductionExportTests::invalidProfileDoesNotPartiallyResolve()
{
    QTemporaryDir directory;
    ProductionExportPlan plan = planFor(directory.path());
    ProductionExportOutput valid = outputFor(
        QStringLiteral("valid"), QStringLiteral("Valid"), QStringLiteral("valid"));
    ProductionExportOutput invalid = outputFor(
        QStringLiteral("invalid"), QStringLiteral("Invalid"), QStringLiteral("invalid"));
    invalid.profile.formatSuffix = QStringLiteral("not-a-format");
    plan.outputs = {valid, invalid};
    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY(!resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error));
    QVERIFY(resolved.isEmpty());
}

void ProductionExportTests::resolvedPayloadValidationAcceptsAutoRenameAndRejectsTampering()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile existing(QDir(directory.path()).filePath(QStringLiteral("same.png")));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("existing");
    existing.close();

    ProductionExportPlan plan = planFor(directory.path());
    plan.collisionPolicy = ProductionExportCollisionPolicy::AutoRename;
    plan.outputs = {outputFor(QStringLiteral("one"), QStringLiteral("One"),
                              QStringLiteral("same"))};

    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY2(resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error), qPrintable(error));
    QCOMPARE(resolved.size(), 1);
    QCOMPARE(QFileInfo(resolved.constFirst().request.filePath).fileName(),
             QStringLiteral("same-2.png"));
    QVERIFY2(validateResolvedProductionExportOutputs(
                 plan, colourState(), resolved, &error), qPrintable(error));

    QVector<ResolvedProductionExportOutput> tampered = resolved;
    tampered[0].request.quality = 7;
    QVERIFY(!validateResolvedProductionExportOutputs(
        plan, colourState(), tampered, &error));
    QVERIFY(error.contains(QStringLiteral("captured export profile"),
                           Qt::CaseInsensitive));

    tampered = resolved;
    tampered[0].request.filePath = QDir::home().filePath(
        QStringLiteral("same-2.png"));
    QVERIFY(!validateResolvedProductionExportOutputs(
        plan, colourState(), tampered, &error));
    QVERIFY(error.contains(QStringLiteral("selected directory"),
                           Qt::CaseInsensitive));
}

void ProductionExportTests::resolvedPayloadValidationRejectsDuplicateDestinations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ProductionExportPlan plan = planFor(directory.path());
    plan.outputs = {
        outputFor(QStringLiteral("one"), QStringLiteral("One"),
                  QStringLiteral("one")),
        outputFor(QStringLiteral("two"), QStringLiteral("Two"),
                  QStringLiteral("two"))
    };

    QVector<ResolvedProductionExportOutput> resolved;
    QString error;
    QVERIFY2(resolveProductionExportPlan(plan, colourState(), &resolved,
                                         nullptr, &error), qPrintable(error));
    QCOMPARE(resolved.size(), 2);
    resolved[1].request.filePath = resolved[0].request.filePath;
    QVERIFY(!validateResolvedProductionExportOutputs(
        plan, colourState(), resolved, &error));
    QVERIFY(error.contains(QStringLiteral("duplicated"),
                           Qt::CaseInsensitive)
            || error.contains(QStringLiteral("filename template"),
                              Qt::CaseInsensitive));
}

QTEST_APPLESS_MAIN(ProductionExportTests)
#include "test_production_export.moc"
