#include "CadModel.hpp"
#include "ExpressionParser.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double actual, double expected, double tolerance = 1.0e-7)
{
    return std::abs(actual - expected) <= std::max(1.0, std::abs(expected)) * tolerance;
}

CadDocument makeParametricDocument()
{
    CadDocument document;
    document.name = QStringLiteral("Sketch and Extrude test");
    document.variables.insert(QStringLiteral("L"), QStringLiteral("120"));
    document.variables.insert(QStringLiteral("W"), QStringLiteral("L / 2"));

    CadObject sketch;
    sketch.id = cadNewId();
    sketch.name = QStringLiteral("Sketch");
    sketch.featureType = QStringLiteral("sketch");
    sketch.primitiveType = QStringLiteral("sketch-rectangle");
    sketch.sketchPlane = QStringLiteral("XY");
    sketch.primitiveParameters = {40.0, 20.0};
    sketch.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("W"));
    sketch.parameterExpressions.insert(QStringLiteral("p1"), QStringLiteral("W / 2"));
    sketch.sketchConstraints = {QStringLiteral("horizontal"), QStringLiteral("vertical"),
                                QStringLiteral("coincident-closed"), QStringLiteral("grid-snap-5mm")};

    CadObject extrude;
    extrude.id = cadNewId();
    extrude.name = QStringLiteral("Extrude");
    extrude.featureType = QStringLiteral("extrude");
    extrude.primitiveType = QStringLiteral("extrude");
    extrude.inputs = {sketch.id};
    extrude.primitiveParameters = {10.0};
    extrude.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("sqrt(100)"));

    document.objects = {sketch, extrude};
    return document;
}

CadObject makeBoxObject(const QString& name, const std::vector<double>& dimensions, const QVector3D& position)
{
    CadObject object;
    object.id = cadNewId();
    object.name = name;
    object.featureType = QStringLiteral("primitive");
    object.primitiveType = QStringLiteral("box");
    object.setPrimitiveParameters(dimensions);
    object.position = position;
    return object;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        CadDocument document = makeParametricDocument();
        CadIO::recomputeParametricFeatures(document);
        require(document.objects.size() == 2, "feature count changed during recompute");
        require(document.objects[0].shape.ShapeType() == TopAbs_WIRE, "rectangle sketch is not a wire");
        require(document.objects[1].inputs.front() == document.objects[0].id, "extrude link was not preserved");

        const CadGeometryProperties properties = CadIO::analyzeShape(document.objects[1].shape);
        require(properties.hasVolume && near(properties.volumeMm3, 18000.0), "extrude volume is incorrect");

        const QVector3D yz = cadSketchPointToWorld(QStringLiteral("YZ"), QVector2D(3.0F, 4.0F));
        require(yz == QVector3D(0.0F, 3.0F, 4.0F), "YZ plane mapping is incorrect");
        const QVector2D zx = cadSketchPointFromWorld(QStringLiteral("ZX"), QVector3D(4.0F, 0.0F, 3.0F));
        require(zx == QVector2D(3.0F, 4.0F), "ZX plane mapping is incorrect");

        const TopoDS_Shape symmetric = CadIO::makeExtrusion(document.objects[0], 10.0, true);
        const CadGeometryProperties symmetricProperties = CadIO::analyzeShape(symmetric);
        require(symmetricProperties.hasVolume && near(symmetricProperties.volumeMm3, 18000.0),
                "symmetric extrude volume is incorrect");

        CadObject booleanBase = makeBoxObject(QStringLiteral("Base"), {10.0, 10.0, 10.0}, QVector3D(0, 0, 0));
        CadObject booleanTool = makeBoxObject(QStringLiteral("Tool"), {10.0, 10.0, 10.0}, QVector3D(5, 0, 0));
        CadObject unionFeature;
        unionFeature.id = cadNewId();
        unionFeature.name = QStringLiteral("Union");
        unionFeature.featureType = QStringLiteral("boolean");
        unionFeature.primitiveType = QStringLiteral("union");
        unionFeature.inputs = {booleanBase.id, booleanTool.id};
        CadObject cutFeature;
        cutFeature.id = cadNewId();
        cutFeature.name = QStringLiteral("Cut");
        cutFeature.featureType = QStringLiteral("boolean");
        cutFeature.primitiveType = QStringLiteral("cut");
        cutFeature.inputs = {booleanBase.id, booleanTool.id};
        CadObject commonFeature;
        commonFeature.id = cadNewId();
        commonFeature.name = QStringLiteral("Common");
        commonFeature.featureType = QStringLiteral("boolean");
        commonFeature.primitiveType = QStringLiteral("common");
        commonFeature.inputs = {booleanBase.id, booleanTool.id};
        CadDocument booleanDocument;
        booleanDocument.objects = {booleanBase, booleanTool, unionFeature, cutFeature, commonFeature};
        CadIO::recomputeParametricFeatures(booleanDocument);
        require(near(CadIO::analyzeShape(booleanDocument.objects[2].shape).volumeMm3, 1500.0),
                "boolean union volume is incorrect");
        require(near(CadIO::analyzeShape(booleanDocument.objects[3].shape).volumeMm3, 500.0),
                "boolean cut volume is incorrect");
        require(near(CadIO::analyzeShape(booleanDocument.objects[4].shape).volumeMm3, 500.0),
                "boolean common volume is incorrect");
        booleanDocument.objects[1].position.setX(3.0F);
        CadIO::recomputeParametricFeatures(booleanDocument);
        require(near(CadIO::analyzeShape(booleanDocument.objects[2].shape).volumeMm3, 1300.0),
                "boolean feature did not update when its input transform changed");
        require(near(CadIO::analyzeShape(booleanDocument.objects[4].shape).volumeMm3, 700.0),
                "boolean intersection did not update when its input transform changed");
        CadDocument failedBooleanDocument = booleanDocument;
        failedBooleanDocument.objects[1].position.setX(20.0F);
        bool emptyIntersectionRejected = false;
        try {
            CadIO::recomputeParametricFeatures(failedBooleanDocument);
        } catch (const Standard_Failure&) {
            emptyIntersectionRejected = true;
        } catch (const std::exception&) {
            emptyIntersectionRejected = true;
        }
        require(emptyIntersectionRejected, "empty boolean intersection was not rejected");
        require(near(CadIO::analyzeShape(failedBooleanDocument.objects[4].shape).volumeMm3, 700.0),
                "failed boolean recompute partially replaced the previous valid result");

        const CadObject edgeTreatmentBase = makeBoxObject(QStringLiteral("Edge treatment base"),
                                                           {30.0, 20.0, 10.0}, QVector3D(0, 0, 0));
        const double originalBoxVolume = CadIO::analyzeShape(edgeTreatmentBase.shape).volumeMm3;
        const TopoDS_Shape filletedBox = CadIO::makeFillet(edgeTreatmentBase.shape, 0.5);
        const TopoDS_Shape chamferedBox = CadIO::makeChamfer(edgeTreatmentBase.shape, 0.5);
        const double filletedVolume = CadIO::analyzeShape(filletedBox).volumeMm3;
        const double chamferedVolume = CadIO::analyzeShape(chamferedBox).volumeMm3;
        require(filletedVolume > 0.0 && filletedVolume < originalBoxVolume,
                "all-edge fillet did not create a valid, smaller solid");
        require(chamferedVolume > 0.0 && chamferedVolume < originalBoxVolume,
                "all-edge chamfer did not create a valid, smaller solid");

        CadObject filletFeature;
        filletFeature.id = cadNewId();
        filletFeature.name = QStringLiteral("Parametric fillet");
        filletFeature.featureType = QStringLiteral("fillet");
        filletFeature.primitiveType = QStringLiteral("fillet");
        filletFeature.inputs = {edgeTreatmentBase.id};
        filletFeature.primitiveParameters = {0.5};
        filletFeature.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("R"));
        CadObject chamferFeature;
        chamferFeature.id = cadNewId();
        chamferFeature.name = QStringLiteral("Parametric chamfer");
        chamferFeature.featureType = QStringLiteral("chamfer");
        chamferFeature.primitiveType = QStringLiteral("chamfer");
        chamferFeature.inputs = {edgeTreatmentBase.id};
        chamferFeature.primitiveParameters = {0.5};
        chamferFeature.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("0.5"));
        CadDocument edgeTreatmentDocument;
        edgeTreatmentDocument.variables.insert(QStringLiteral("R"), QStringLiteral("0.5"));
        edgeTreatmentDocument.objects = {edgeTreatmentBase, filletFeature, chamferFeature};
        CadIO::recomputeParametricFeatures(edgeTreatmentDocument);
        require(near(CadIO::analyzeShape(edgeTreatmentDocument.objects[1].shape).volumeMm3, filletedVolume),
                "parametric fillet feature did not match the kernel result");
        require(near(CadIO::analyzeShape(edgeTreatmentDocument.objects[2].shape).volumeMm3, chamferedVolume),
                "parametric chamfer feature did not match the kernel result");

        CadObject patternBase = makeBoxObject(QStringLiteral("Pattern source"),
                                               {10.0, 10.0, 10.0}, QVector3D(2, 0, 0));
        CadObject linearPattern;
        linearPattern.id = cadNewId();
        linearPattern.name = QStringLiteral("Linear pattern");
        linearPattern.featureType = QStringLiteral("pattern");
        linearPattern.primitiveType = QStringLiteral("linear-pattern-x");
        linearPattern.inputs = {patternBase.id};
        linearPattern.primitiveParameters = {3.0, 20.0};
        linearPattern.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("Copies"));
        linearPattern.parameterExpressions.insert(QStringLiteral("p1"), QStringLiteral("20"));
        CadObject mirrorFeature;
        mirrorFeature.id = cadNewId();
        mirrorFeature.name = QStringLiteral("Mirror copy");
        mirrorFeature.featureType = QStringLiteral("mirror");
        mirrorFeature.primitiveType = QStringLiteral("mirror-yz");
        mirrorFeature.inputs = {patternBase.id};
        CadDocument patternDocument;
        patternDocument.variables.insert(QStringLiteral("Copies"), QStringLiteral("3"));
        patternDocument.objects = {patternBase, linearPattern, mirrorFeature};
        CadIO::recomputeParametricFeatures(patternDocument);
        const CadGeometryProperties patternProperties = CadIO::analyzeShape(patternDocument.objects[1].shape);
        const CadGeometryProperties mirrorProperties = CadIO::analyzeShape(patternDocument.objects[2].shape);
        require(near(patternProperties.volumeMm3, 3000.0) && patternProperties.hasBounds &&
                    near(patternProperties.boundsMm.x(), 50.0),
                "linear pattern count, volume, or bounds are incorrect");
        require(near(mirrorProperties.volumeMm3, 2000.0) && mirrorProperties.hasBounds &&
                    near(mirrorProperties.boundsMm.x(), 24.0),
                "YZ mirror-copy volume or bounds are incorrect");
        patternDocument.variables[QStringLiteral("Copies")] = QStringLiteral("4");
        CadIO::recomputeParametricFeatures(patternDocument);
        require(near(CadIO::analyzeShape(patternDocument.objects[1].shape).volumeMm3, 4000.0),
                "linear pattern did not recompute after its count variable changed");
        CadDocument invalidPatternDocument = patternDocument;
        invalidPatternDocument.variables[QStringLiteral("Copies")] = QStringLiteral("2.5");
        bool fractionalPatternCountRejected = false;
        try {
            CadIO::recomputeParametricFeatures(invalidPatternDocument);
        } catch (const std::exception&) {
            fractionalPatternCountRejected = true;
        }
        require(fractionalPatternCountRejected, "fractional linear pattern count was not rejected");
        require(near(CadIO::analyzeShape(invalidPatternDocument.objects[1].shape).volumeMm3, 4000.0),
                "invalid pattern count partially replaced the previous valid result");

        CadObject circle;
        circle.id = cadNewId();
        circle.name = QStringLiteral("Circle on YZ");
        circle.featureType = QStringLiteral("sketch");
        circle.primitiveType = QStringLiteral("sketch-circle");
        circle.sketchPlane = QStringLiteral("YZ");
        circle.sketchOrigin = QVector2D(5.0F, -3.0F);
        circle.primitiveParameters = {10.0};
        circle.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("10"));
        CadDocument circleDocument;
        circleDocument.objects.push_back(circle);
        CadIO::recomputeParametricFeatures(circleDocument);
        const TopoDS_Shape cylinder = CadIO::makeExtrusion(circleDocument.objects.front(), 5.0);
        const CadGeometryProperties cylinderProperties = CadIO::analyzeShape(cylinder);
        require(near(cylinderProperties.volumeMm3, std::acos(-1.0) * 10.0 * 10.0 * 5.0, 1.0e-6),
                "circle profile extrusion volume is incorrect");
        require(cylinderProperties.hasBounds && near(cylinderProperties.boundsMm.x(), 5.0, 1.0e-5) &&
                    near(cylinderProperties.boundsMm.y(), 20.0, 1.0e-5) &&
                    near(cylinderProperties.boundsMm.z(), 20.0, 1.0e-5),
                "YZ circle extrusion orientation or bounds are incorrect");

        QTemporaryDir temporaryDirectory;
        require(temporaryDirectory.isValid(), "could not create temporary directory");
        const QString projectPath = temporaryDirectory.filePath(QStringLiteral("roundtrip.aoscad"));
        CadIO::saveProject(document, projectPath);
        const CadDocument loaded = CadIO::loadProject(projectPath);
        require(loaded.variables == document.variables, "variables were not saved and loaded");
        require(loaded.objects.size() == 2, "round-trip feature count is incorrect");
        require(loaded.objects[1].inputs == document.objects[1].inputs, "round-trip feature link is incorrect");
        require(loaded.objects[0].parameterExpressions == document.objects[0].parameterExpressions,
                "round-trip sketch formulas are incorrect");
        require(near(CadIO::analyzeShape(loaded.objects[1].shape).volumeMm3, 18000.0),
                "round-trip B-Rep volume is incorrect");

        const QString booleanProjectPath = temporaryDirectory.filePath(QStringLiteral("boolean-features.aoscad"));
        CadIO::saveProject(booleanDocument, booleanProjectPath);
        const CadDocument loadedBooleans = CadIO::loadProject(booleanProjectPath);
        require(loadedBooleans.objects.size() == booleanDocument.objects.size() &&
                    loadedBooleans.objects[2].primitiveType == QStringLiteral("union") &&
                    near(CadIO::analyzeShape(loadedBooleans.objects[2].shape).volumeMm3, 1300.0) &&
                    near(CadIO::analyzeShape(loadedBooleans.objects[4].shape).volumeMm3, 700.0),
                "boolean feature graph did not survive project round-trip");

        const QString edgeProjectPath = temporaryDirectory.filePath(QStringLiteral("edge-features.aoscad"));
        CadIO::saveProject(edgeTreatmentDocument, edgeProjectPath);
        const CadDocument loadedEdgeFeatures = CadIO::loadProject(edgeProjectPath);
        require(loadedEdgeFeatures.objects.size() == edgeTreatmentDocument.objects.size() &&
                    near(CadIO::analyzeShape(loadedEdgeFeatures.objects[1].shape).volumeMm3, filletedVolume) &&
                    near(CadIO::analyzeShape(loadedEdgeFeatures.objects[2].shape).volumeMm3, chamferedVolume),
                "fillet or chamfer feature did not survive project round-trip");

        const QString patternProjectPath = temporaryDirectory.filePath(QStringLiteral("pattern-features.aoscad"));
        CadIO::saveProject(patternDocument, patternProjectPath);
        const CadDocument loadedPatterns = CadIO::loadProject(patternProjectPath);
        require(loadedPatterns.variables.value(QStringLiteral("Copies")) == QStringLiteral("4") &&
                    loadedPatterns.objects[2].primitiveType == QStringLiteral("mirror-yz") &&
                    near(CadIO::analyzeShape(loadedPatterns.objects[1].shape).volumeMm3, 4000.0) &&
                    near(CadIO::analyzeShape(loadedPatterns.objects[2].shape).volumeMm3, 2000.0),
                "pattern and mirror features did not survive project round-trip");

        bool cycleDetected = false;
        CadDocument cyclic = document;
        CadObject precedingPrimitive;
        precedingPrimitive.id = cadNewId();
        precedingPrimitive.name = QStringLiteral("Preceding primitive");
        precedingPrimitive.featureType = QStringLiteral("primitive");
        precedingPrimitive.primitiveType = QStringLiteral("box");
        precedingPrimitive.setPrimitiveParameters({1.0, 1.0, 1.0});
        cyclic.objects.insert(cyclic.objects.begin(), precedingPrimitive);
        cyclic.objects[0].primitiveParameters = {2.0, 2.0, 2.0};
        cyclic.objects[1].inputs = {cyclic.objects[2].id};
        cyclic.objects[2].inputs = {cyclic.objects[1].id};
        try {
            CadIO::recomputeParametricFeatures(cyclic);
        } catch (const std::exception&) {
            cycleDetected = true;
        }
        require(cycleDetected, "feature graph cycle was not rejected");
        require(near(CadIO::analyzeShape(cyclic.objects[0].shape).volumeMm3, 1.0),
                "failed recompute partially changed a preceding feature");
        require(near(CadIO::analyzeShape(cyclic.objects[2].shape).volumeMm3, 18000.0),
                "failed recompute partially changed the extrusion");

        std::cout << "CAD model integration tests passed\n";
        return 0;
    } catch (const Standard_Failure& error) {
        std::cerr << "Open CASCADE test failure: " << error.GetMessageString() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "CAD model test failure: " << error.what() << '\n';
    }
    return 1;
}
