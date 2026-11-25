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
