#include "CadModel.hpp"
#include "ExpressionParser.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
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

        CadObject revolveSketch;
        revolveSketch.id = cadNewId();
        revolveSketch.name = QStringLiteral("Revolve profile");
        revolveSketch.featureType = QStringLiteral("sketch");
        revolveSketch.primitiveType = QStringLiteral("sketch-rectangle");
        revolveSketch.sketchPlane = QStringLiteral("XY");
        revolveSketch.sketchOrigin = QVector2D(5.0F, 0.0F);
        revolveSketch.primitiveParameters = {5.0, 20.0};
        CadObject revolveFeature;
        revolveFeature.id = cadNewId();
        revolveFeature.name = QStringLiteral("Revolve");
        revolveFeature.featureType = QStringLiteral("revolve");
        revolveFeature.primitiveType = QStringLiteral("revolve-y");
        revolveFeature.inputs = {revolveSketch.id};
        revolveFeature.primitiveParameters = {360.0};
        revolveFeature.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("Angle"));
        CadDocument revolveDocument;
        revolveDocument.variables.insert(QStringLiteral("Angle"), QStringLiteral("360"));
        revolveDocument.objects = {revolveSketch, revolveFeature};
        CadIO::recomputeParametricFeatures(revolveDocument);
        const double fullRevolveVolume = 1500.0 * std::acos(-1.0);
        require(near(CadIO::analyzeShape(revolveDocument.objects[1].shape).volumeMm3, fullRevolveVolume, 1.0e-6),
                "full revolution volume is incorrect");
        revolveDocument.variables[QStringLiteral("Angle")] = QStringLiteral("180");
        CadIO::recomputeParametricFeatures(revolveDocument);
        require(near(CadIO::analyzeShape(revolveDocument.objects[1].shape).volumeMm3, fullRevolveVolume * 0.5, 1.0e-6),
                "parametric revolution did not update from its angle variable");

        const CadObject holeBase = makeBoxObject(QStringLiteral("Hole base"), {20.0, 20.0, 20.0}, QVector3D(0, 0, 0));
        const double expectedHoleVolume = 8000.0 - 80.0 * std::acos(-1.0);
        for (const QString& axis : {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")}) {
            const TopoDS_Shape hole = CadIO::makeThroughHole(holeBase.worldShape(), axis, 4.0, 10.0, 10.0);
            require(near(CadIO::analyzeShape(hole).volumeMm3, expectedHoleVolume, 1.0e-6),
                    "through-hole volume is incorrect for an axis");
        }
        CadObject holeFeature;
        holeFeature.id = cadNewId();
        holeFeature.name = QStringLiteral("Parametric through-hole");
        holeFeature.featureType = QStringLiteral("hole");
        holeFeature.primitiveType = QStringLiteral("hole-through-z");
        holeFeature.inputs = {holeBase.id};
        holeFeature.primitiveParameters = {4.0, 10.0, 10.0};
        holeFeature.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("Diameter"));
        holeFeature.parameterExpressions.insert(QStringLiteral("p1"), QStringLiteral("10"));
        holeFeature.parameterExpressions.insert(QStringLiteral("p2"), QStringLiteral("10"));
        CadDocument holeDocument;
        holeDocument.variables.insert(QStringLiteral("Diameter"), QStringLiteral("4"));
        holeDocument.objects = {holeBase, holeFeature};
        CadIO::recomputeParametricFeatures(holeDocument);
        require(near(CadIO::analyzeShape(holeDocument.objects[1].shape).volumeMm3, expectedHoleVolume, 1.0e-6),
                "parametric through-hole feature volume is incorrect");
        holeDocument.variables[QStringLiteral("Diameter")] = QStringLiteral("6");
        CadIO::recomputeParametricFeatures(holeDocument);
        require(near(CadIO::analyzeShape(holeDocument.objects[1].shape).volumeMm3,
                     8000.0 - 180.0 * std::acos(-1.0), 1.0e-6),
                "through-hole did not update after its diameter variable changed");
        bool missedHoleRejected = false;
        try {
            CadIO::makeThroughHole(holeBase.worldShape(), QStringLiteral("Z"), 4.0, 50.0, 50.0);
        } catch (const std::exception&) {
            missedHoleRejected = true;
        }
        require(missedHoleRejected, "through-hole outside the source body was not rejected");

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
        require(near(CadIO::intersectionVolumeMm3(booleanDocument.objects[0].worldShape(),
                                                  booleanDocument.objects[1].worldShape()), 500.0),
                "intersection diagnostic returned an incorrect overlap volume");
        const CadObject disjointTool = makeBoxObject(QStringLiteral("Disjoint tool"),
                                                      {10.0, 10.0, 10.0}, QVector3D(20, 0, 0));
        require(near(CadIO::intersectionVolumeMm3(booleanDocument.objects[0].worldShape(),
                                                  disjointTool.worldShape()), 0.0),
                "intersection diagnostic reported volume for disjoint bodies");
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

        const CadObject propertyBox = makeBoxObject(QStringLiteral("Mass properties"),
                                                     {2.0, 4.0, 6.0}, QVector3D(0, 0, 0));
        const CadGeometryProperties massProperties = CadIO::analyzeShape(propertyBox.worldShape(), true);
        require(massProperties.hasInertia &&
                    near(massProperties.centerOfMassMm[0], 1.0) &&
                    near(massProperties.centerOfMassMm[1], 2.0) &&
                    near(massProperties.centerOfMassMm[2], 3.0) &&
                    near(massProperties.inertiaTensorDiagonalMm5[0], 208.0) &&
                    near(massProperties.inertiaTensorDiagonalMm5[1], 160.0) &&
                    near(massProperties.inertiaTensorDiagonalMm5[2], 80.0),
                "center of mass or inertia tensor is incorrect for a box");

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
        CadObject circularPattern;
        circularPattern.id = cadNewId();
        circularPattern.name = QStringLiteral("Circular pattern");
        circularPattern.featureType = QStringLiteral("pattern");
        circularPattern.primitiveType = QStringLiteral("circular-pattern-z");
        circularPattern.inputs = {patternBase.id};
        circularPattern.primitiveParameters = {4.0, 90.0};
        circularPattern.parameterExpressions.insert(QStringLiteral("p0"), QStringLiteral("CircularCopies"));
        circularPattern.parameterExpressions.insert(QStringLiteral("p1"), QStringLiteral("90"));
        CadDocument patternDocument;
        patternDocument.variables.insert(QStringLiteral("Copies"), QStringLiteral("3"));
        patternDocument.variables.insert(QStringLiteral("CircularCopies"), QStringLiteral("4"));
        patternDocument.objects = {patternBase, linearPattern, mirrorFeature, circularPattern};
        CadIO::recomputeParametricFeatures(patternDocument);
        const CadGeometryProperties patternProperties = CadIO::analyzeShape(patternDocument.objects[1].shape);
        const CadGeometryProperties mirrorProperties = CadIO::analyzeShape(patternDocument.objects[2].shape);
        const CadGeometryProperties circularPatternProperties = CadIO::analyzeShape(patternDocument.objects[3].shape);
        require(near(patternProperties.volumeMm3, 3000.0) && patternProperties.hasBounds &&
                    near(patternProperties.boundsMm.x(), 50.0),
                "linear pattern count, volume, or bounds are incorrect");
        require(near(mirrorProperties.volumeMm3, 2000.0) && mirrorProperties.hasBounds &&
                    near(mirrorProperties.boundsMm.x(), 24.0),
                "YZ mirror-copy volume or bounds are incorrect");
        require(near(circularPatternProperties.volumeMm3, 4000.0) && circularPatternProperties.hasBounds &&
                    near(circularPatternProperties.boundsMm.x(), 24.0) &&
                    near(circularPatternProperties.boundsMm.y(), 24.0),
                "circular pattern count, volume, or bounds are incorrect");
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
        patternDocument.variables[QStringLiteral("CircularCopies")] = QStringLiteral("3");
        CadIO::recomputeParametricFeatures(patternDocument);
        require(near(CadIO::analyzeShape(patternDocument.objects[3].shape).volumeMm3, 3000.0),
                "circular pattern did not recompute after its count variable changed");
        patternDocument.variables[QStringLiteral("CircularCopies")] = QStringLiteral("4");
        CadIO::recomputeParametricFeatures(patternDocument);
        CadDocument invalidCircularPatternDocument = patternDocument;
        invalidCircularPatternDocument.objects[3].parameterExpressions[QStringLiteral("p1")] = QStringLiteral("100");
        bool excessiveCircularSweepRejected = false;
        try {
            CadIO::recomputeParametricFeatures(invalidCircularPatternDocument);
        } catch (const std::exception&) {
            excessiveCircularSweepRejected = true;
        }
        require(excessiveCircularSweepRejected, "circular pattern covering more than 360 degrees was not rejected");
        require(near(CadIO::analyzeShape(invalidCircularPatternDocument.objects[3].shape).volumeMm3, 4000.0),
                "invalid circular-pattern recompute partially replaced its previous result");

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

        const QString asciiPlyPath = temporaryDirectory.filePath(QStringLiteral("tetrahedron.ply"));
        QFile asciiPlyFile(asciiPlyPath);
        require(asciiPlyFile.open(QIODevice::WriteOnly), "could not create ASCII PLY fixture");
        const QByteArray asciiPlyData =
            "ply\nformat ascii 1.0\ncomment tetrahedron fixture\n"
            "element vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
            "element face 4\nproperty list uchar int vertex_indices\nend_header\n"
            "0 0 0\n1 0 0\n0 1 0\n0 0 1\n"
            "3 0 2 1\n3 0 1 3\n3 0 3 2\n3 1 2 3\n";
        require(asciiPlyFile.write(asciiPlyData) == asciiPlyData.size(), "could not write ASCII PLY fixture");
        asciiPlyFile.close();
        const CadObject asciiPlyObject = CadIO::importFile(asciiPlyPath);
        require(asciiPlyObject.featureType == QStringLiteral("imported-body") && !asciiPlyObject.shape.IsNull(),
                "ASCII PLY did not import as a mesh body");
        require(!CadIO::triangulate(asciiPlyObject.shape, 0).empty(), "ASCII PLY imported without renderable triangles");

        const QString binaryPlyPath = temporaryDirectory.filePath(QStringLiteral("triangle-binary.ply"));
        QFile binaryPlyFile(binaryPlyPath);
        require(binaryPlyFile.open(QIODevice::WriteOnly), "could not create binary PLY fixture");
        const QByteArray binaryPlyHeader =
            "ply\nformat binary_little_endian 1.0\n"
            "element vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
            "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
        require(binaryPlyFile.write(binaryPlyHeader) == binaryPlyHeader.size(), "could not write binary PLY header");
        QDataStream binaryPlyData(&binaryPlyFile);
        binaryPlyData.setByteOrder(QDataStream::LittleEndian);
        binaryPlyData.setFloatingPointPrecision(QDataStream::SinglePrecision);
        binaryPlyData << 0.0F << 0.0F << 0.0F
                      << 1.0F << 0.0F << 0.0F
                      << 0.0F << 1.0F << 0.0F
                      << quint8(3) << qint32(0) << qint32(1) << qint32(2);
        require(binaryPlyData.status() == QDataStream::Ok, "could not write binary PLY data");
        binaryPlyFile.close();
        const CadObject binaryPlyObject = CadIO::importFile(binaryPlyPath);
        require(binaryPlyObject.featureType == QStringLiteral("imported-body") &&
                    !CadIO::triangulate(binaryPlyObject.shape, 0).empty(),
                "binary little-endian PLY did not import as a renderable mesh");

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
                    loadedPatterns.objects[3].primitiveType == QStringLiteral("circular-pattern-z") &&
                    near(CadIO::analyzeShape(loadedPatterns.objects[1].shape).volumeMm3, 4000.0) &&
                    near(CadIO::analyzeShape(loadedPatterns.objects[2].shape).volumeMm3, 2000.0) &&
                    near(CadIO::analyzeShape(loadedPatterns.objects[3].shape).volumeMm3, 4000.0),
                "linear, circular-pattern, or mirror features did not survive project round-trip");

        const QString revolveProjectPath = temporaryDirectory.filePath(QStringLiteral("revolve-feature.aoscad"));
        CadIO::saveProject(revolveDocument, revolveProjectPath);
        const CadDocument loadedRevolve = CadIO::loadProject(revolveProjectPath);
        require(loadedRevolve.variables.value(QStringLiteral("Angle")) == QStringLiteral("180") &&
                    loadedRevolve.objects[1].primitiveType == QStringLiteral("revolve-y") &&
                    near(CadIO::analyzeShape(loadedRevolve.objects[1].shape).volumeMm3, fullRevolveVolume * 0.5, 1.0e-6),
                "revolution feature or angle formula did not survive project round-trip");

        const QString holeProjectPath = temporaryDirectory.filePath(QStringLiteral("hole-feature.aoscad"));
        CadIO::saveProject(holeDocument, holeProjectPath);
        const CadDocument loadedHole = CadIO::loadProject(holeProjectPath);
        require(loadedHole.variables.value(QStringLiteral("Diameter")) == QStringLiteral("6") &&
                    loadedHole.objects[1].primitiveType == QStringLiteral("hole-through-z") &&
                    near(CadIO::analyzeShape(loadedHole.objects[1].shape).volumeMm3,
                         8000.0 - 180.0 * std::acos(-1.0), 1.0e-6),
                "through-hole feature or dimensions did not survive project round-trip");

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
