#include "CadModel.hpp"
#include "ExpressionParser.hpp"

#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <IGESControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangle.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_StepModelType.hxx>
#include <STEPControl_Writer.hxx>
#include <StlAPI_Reader.hxx>
#include <StlAPI_Writer.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <gp_XYZ.hxx>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>

namespace {

QString extensionOf(const QString& path)
{
    return QFileInfo(path).suffix().toLower();
}

QByteArray nativePath(const QString& path)
{
    return QFile::encodeName(path);
}

TopoDS_Shape readStep(const QString& path)
{
    STEPControl_Reader reader;
    if (reader.ReadFile(nativePath(path).constData()) != IFSelect_RetDone)
        throw CadError(QStringLiteral("Open CASCADE не смог прочитать STEP-файл."));
    if (reader.TransferRoots() == 0)
        throw CadError(QStringLiteral("В STEP-файле не найдены CAD-объекты."));
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull())
        throw CadError(QStringLiteral("STEP-файл не содержит геометрии."));
    return shape;
}

TopoDS_Shape readIges(const QString& path)
{
    IGESControl_Reader reader;
    if (reader.ReadFile(nativePath(path).constData()) != IFSelect_RetDone)
        throw CadError(QStringLiteral("Open CASCADE не смог прочитать IGES-файл."));
    if (reader.TransferRoots() == 0)
        throw CadError(QStringLiteral("В IGES-файле не найдены CAD-объекты."));
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull())
        throw CadError(QStringLiteral("IGES-файл не содержит геометрии."));
    return shape;
}

TopoDS_Shape readBrep(const QString& path)
{
    TopoDS_Shape shape;
    BRep_Builder builder;
    if (!BRepTools::Read(shape, nativePath(path).constData(), builder) || shape.IsNull())
        throw CadError(QStringLiteral("Open CASCADE не смог прочитать BREP-файл."));
    return shape;
}

TopoDS_Shape readStl(const QString& path)
{
    TopoDS_Shape shape;
    StlAPI_Reader reader;
    if (!reader.Read(shape, nativePath(path).constData()) || shape.IsNull())
        throw CadError(QStringLiteral("Не удалось прочитать STL-файл."));
    return shape;
}

TopoDS_Shape readObj(const QString& path)
{
    std::ifstream input(nativePath(path).constData());
    if (!input)
        throw CadError(QStringLiteral("Не удалось открыть OBJ-файл."));

    std::vector<gp_Pnt> vertices;
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);

    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string kind;
        row >> kind;
        if (kind == "v") {
            double x = 0.0, y = 0.0, z = 0.0;
            if (row >> x >> y >> z)
                vertices.emplace_back(x, y, z);
        } else if (kind == "f") {
            std::vector<std::size_t> faceIndices;
            std::string token;
            while (row >> token) {
                const std::size_t slash = token.find('/');
                const std::string indexText = token.substr(0, slash);
                if (indexText.empty())
                    continue;
                const long rawIndex = std::stol(indexText);
                const long resolved = rawIndex > 0
                    ? rawIndex - 1
                    : static_cast<long>(vertices.size()) + rawIndex;
                if (resolved < 0 || static_cast<std::size_t>(resolved) >= vertices.size())
                    throw CadError(QStringLiteral("OBJ содержит некорректную ссылку на вершину."));
                faceIndices.push_back(static_cast<std::size_t>(resolved));
            }
            if (faceIndices.size() < 3)
                continue;

            for (std::size_t i = 1; i + 1 < faceIndices.size(); ++i) {
                BRepBuilderAPI_MakePolygon polygon;
                polygon.Add(vertices[faceIndices[0]]);
                polygon.Add(vertices[faceIndices[i]]);
                polygon.Add(vertices[faceIndices[i + 1]]);
                polygon.Close();
                if (!polygon.IsDone())
                    continue;
                BRepBuilderAPI_MakeFace face(polygon.Wire());
                if (face.IsDone())
                    builder.Add(compound, face.Face());
            }
        }
    }

    if (compound.IsNull() || TopExp_Explorer(compound, TopAbs_FACE).More() == false)
        throw CadError(QStringLiteral("В OBJ не найдены пригодные грани."));
    return compound;
}

QJsonArray vectorToJson(const QVector3D& value)
{
    return QJsonArray{value.x(), value.y(), value.z()};
}

QVector3D vectorFromJson(const QJsonValue& value, const QVector3D& fallback)
{
    if (!value.isArray())
        return fallback;
    const QJsonArray array = value.toArray();
    if (array.size() != 3)
        return fallback;
    return QVector3D(static_cast<float>(array[0].toDouble(fallback.x())),
                     static_cast<float>(array[1].toDouble(fallback.y())),
                     static_cast<float>(array[2].toDouble(fallback.z())));
}

std::vector<double> evaluatedParameters(const CadObject& object,
                                        const ExpressionParser::Values& variables)
{
    const int count = primitiveParameterCount(object.primitiveType);
    if (count <= 0 || count != static_cast<int>(object.primitiveParameters.size()))
        throw CadError(QStringLiteral("Параметры фичи «%1» имеют некорректный размер.").arg(object.name));

    std::vector<double> result = object.primitiveParameters;
    for (int i = 0; i < count; ++i) {
        const QString key = QStringLiteral("p%1").arg(i);
        const QString expression = object.parameterExpressions.value(key).trimmed();
        if (!expression.isEmpty())
            result[static_cast<std::size_t>(i)] = ExpressionParser::evaluate(expression, variables);
    }
    return result;
}

TopoDS_Shape makePrimitiveShape(const QString& primitiveType,
                                const std::vector<double>& parameters,
                                const QString& sketchPlane = QStringLiteral("XY"),
                                const QVector2D& sketchOrigin = {})
{
    const int expectedCount = primitiveParameterCount(primitiveType);
    if (expectedCount == 0 || static_cast<int>(parameters.size()) != expectedCount)
        throw CadError(QStringLiteral("Набор параметров примитива некорректен."));

    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const double value = parameters[i];
        const bool zeroAllowed = primitiveType == QStringLiteral("cone") && i == 1;
        if (!std::isfinite(value) || value < 0.0 || (!zeroAllowed && value <= 0.0))
            throw CadError(QStringLiteral("Размеры примитива должны быть положительными числами."));
    }

    if (primitiveType == QStringLiteral("sketch-rectangle")) {
        const double width = parameters[0];
        const double height = parameters[1];
        const QVector2D corners[]{
            sketchOrigin,
            sketchOrigin + QVector2D(static_cast<float>(width), 0.0F),
            sketchOrigin + QVector2D(static_cast<float>(width), static_cast<float>(height)),
            sketchOrigin + QVector2D(0.0F, static_cast<float>(height))
        };
        BRepBuilderAPI_MakePolygon polygon;
        for (const QVector2D& corner : corners) {
            const QVector3D point = cadSketchPointToWorld(sketchPlane, corner);
            polygon.Add(gp_Pnt(point.x(), point.y(), point.z()));
        }
        polygon.Close();
        if (!polygon.IsDone() || polygon.Wire().IsNull())
            throw CadError(QStringLiteral("Не удалось построить замкнутый прямоугольный эскиз."));
        return polygon.Wire();
    }
    if (primitiveType == QStringLiteral("sketch-circle")) {
        const QVector3D center = cadSketchPointToWorld(sketchPlane, sketchOrigin);
        const QVector3D normal = cadSketchPlaneNormal(sketchPlane);
        const gp_Circ circle(gp_Ax2(gp_Pnt(center.x(), center.y(), center.z()),
                                   gp_Dir(normal.x(), normal.y(), normal.z())), parameters[0]);
        BRepBuilderAPI_MakeEdge edge(circle);
        if (!edge.IsDone())
            throw CadError(QStringLiteral("Не удалось построить окружность эскиза."));
        BRepBuilderAPI_MakeWire wire(edge.Edge());
        if (!wire.IsDone())
            throw CadError(QStringLiteral("Не удалось замкнуть окружность эскиза."));
        return wire.Wire();
    }
    if (primitiveType == QStringLiteral("box"))
        return BRepPrimAPI_MakeBox(parameters[0], parameters[1], parameters[2]).Shape();
    if (primitiveType == QStringLiteral("cylinder"))
        return BRepPrimAPI_MakeCylinder(parameters[0], parameters[1]).Shape();
    if (primitiveType == QStringLiteral("sphere"))
        return BRepPrimAPI_MakeSphere(parameters[0]).Shape();
    if (primitiveType == QStringLiteral("cone"))
        return BRepPrimAPI_MakeCone(parameters[0], parameters[1], parameters[2]).Shape();
    if (primitiveType == QStringLiteral("torus")) {
        if (parameters[1] >= parameters[0])
            throw CadError(QStringLiteral("Малый радиус тора должен быть меньше большого радиуса."));
        return BRepPrimAPI_MakeTorus(parameters[0], parameters[1]).Shape();
    }
    throw CadError(QStringLiteral("Этот тип примитива не поддерживается."));
}

QByteArray shapeToBase64(const TopoDS_Shape& shape, QTemporaryDir& directory, std::size_t index)
{
    const QString path = directory.filePath(QStringLiteral("shape-%1.brep").arg(index));
    const QByteArray encodedPath = nativePath(path);
    BRepTools::Write(shape, encodedPath.constData());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw CadError(QStringLiteral("Не удалось сериализовать геометрию проекта."));
    const QByteArray serialized = file.readAll();
    if (serialized.isEmpty())
        throw CadError(QStringLiteral("Не удалось сериализовать геометрию проекта."));
    return serialized.toBase64();
}

TopoDS_Shape shapeFromBase64(const QByteArray& encoded, QTemporaryDir& directory, std::size_t index)
{
    const QByteArray bytes = QByteArray::fromBase64(encoded);
    if (bytes.isEmpty())
        throw CadError(QStringLiteral("Файл проекта содержит пустую геометрию."));

    const QString path = directory.filePath(QStringLiteral("shape-%1.brep").arg(index));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw CadError(QStringLiteral("Не удалось распаковать геометрию проекта."));
    file.close();

    TopoDS_Shape shape;
    BRep_Builder builder;
    if (!BRepTools::Read(shape, nativePath(path).constData(), builder) || shape.IsNull())
        throw CadError(QStringLiteral("В проекте AoSCAD повреждена геометрия."));
    return shape;
}

TopoDS_Shape compoundFor(const CadDocument& document, std::optional<std::size_t> onlyObject)
{
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);

    for (std::size_t i = 0; i < document.objects.size(); ++i) {
        if (onlyObject && i != *onlyObject)
            continue;
        const CadObject& object = document.objects[i];
        if (!object.visible || object.suppressed || object.featureType == QStringLiteral("sketch") ||
            object.shape.IsNull())
            continue;
        builder.Add(compound, object.worldShape());
    }
    if (TopExp_Explorer(compound, TopAbs_FACE).More() == false)
        throw CadError(QStringLiteral("Нет видимой геометрии для экспорта."));
    return compound;
}

void writeObj(const CadDocument& document, const QString& filePath, std::optional<std::size_t> onlyObject)
{
    std::ofstream output(nativePath(filePath).constData(), std::ios::binary);
    if (!output)
        throw CadError(QStringLiteral("Не удалось создать OBJ-файл."));

    output << "# AoSCAD Studio mesh export\n";
    std::size_t nextVertex = 1;
    bool wroteAny = false;
    for (std::size_t i = 0; i < document.objects.size(); ++i) {
        if (onlyObject && i != *onlyObject)
            continue;
        const CadObject& object = document.objects[i];
        if (!object.visible || object.suppressed || object.featureType == QStringLiteral("sketch") ||
            object.shape.IsNull())
            continue;
        output << "o " << object.name.toUtf8().constData() << '\n';
        const auto triangles = CadIO::triangulate(object.worldShape(), i);
        for (const CadTriangle& triangle : triangles) {
            const QVector3D points[]{triangle.a, triangle.b, triangle.c};
            for (const QVector3D& p : points)
                output << "v " << p.x() << ' ' << p.y() << ' ' << p.z() << '\n';
            output << "f " << nextVertex << ' ' << nextVertex + 1 << ' ' << nextVertex + 2 << '\n';
            nextVertex += 3;
            wroteAny = true;
        }
    }
    if (!wroteAny)
        throw CadError(QStringLiteral("Нет видимой геометрии для экспорта."));
    if (!output)
        throw CadError(QStringLiteral("Ошибка записи OBJ-файла."));
}

double positiveVolumeOfSolids(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
        return 0.0;

    double totalVolume = 0.0;
    const auto addSolidVolume = [&totalVolume](const TopoDS_Shape& solid) {
        GProp_GProps volumeProperties;
        BRepGProp::VolumeProperties(solid, volumeProperties, true);
        const double volume = std::abs(volumeProperties.Mass());
        if (std::isfinite(volume))
            totalVolume += volume;
    };
    if (shape.ShapeType() == TopAbs_SOLID) {
        addSolidVolume(shape);
    } else {
        for (TopExp_Explorer explorer(shape, TopAbs_SOLID); explorer.More(); explorer.Next())
            addSolidVolume(explorer.Current());
    }
    return std::isfinite(totalVolume) ? totalVolume : 0.0;
}

} // namespace

QString cadNewId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVector3D cadSketchPointToWorld(const QString& plane, const QVector2D& point)
{
    const QString normalized = plane.trimmed().toUpper();
    if (normalized == QStringLiteral("XY"))
        return QVector3D(point.x(), point.y(), 0.0F);
    if (normalized == QStringLiteral("YZ"))
        return QVector3D(0.0F, point.x(), point.y());
    if (normalized == QStringLiteral("ZX"))
        return QVector3D(point.y(), 0.0F, point.x());
    throw CadError(QStringLiteral("Опорная плоскость «%1» не поддерживается.").arg(plane));
}

QVector2D cadSketchPointFromWorld(const QString& plane, const QVector3D& point)
{
    const QString normalized = plane.trimmed().toUpper();
    if (normalized == QStringLiteral("XY"))
        return QVector2D(point.x(), point.y());
    if (normalized == QStringLiteral("YZ"))
        return QVector2D(point.y(), point.z());
    if (normalized == QStringLiteral("ZX"))
        return QVector2D(point.z(), point.x());
    throw CadError(QStringLiteral("Опорная плоскость «%1» не поддерживается.").arg(plane));
}

QVector3D cadSketchPlaneNormal(const QString& plane)
{
    const QString normalized = plane.trimmed().toUpper();
    if (normalized == QStringLiteral("XY"))
        return QVector3D(0.0F, 0.0F, 1.0F);
    if (normalized == QStringLiteral("YZ"))
        return QVector3D(1.0F, 0.0F, 0.0F);
    if (normalized == QStringLiteral("ZX"))
        return QVector3D(0.0F, 1.0F, 0.0F);
    throw CadError(QStringLiteral("Опорная плоскость «%1» не поддерживается.").arg(plane));
}

CadError::CadError(const QString& message)
    : std::runtime_error(message.toUtf8().constData())
{
}

const std::vector<CadMaterial>& cadMaterials()
{
    static const std::vector<CadMaterial> materials{
        {QStringLiteral("unassigned"), QStringLiteral("Не задан"), 0.0},
        {QStringLiteral("steel"), QStringLiteral("Сталь"), 7850.0},
        {QStringLiteral("stainless-steel"), QStringLiteral("Нержавеющая сталь"), 8000.0},
        {QStringLiteral("aluminum"), QStringLiteral("Алюминий"), 2700.0},
        {QStringLiteral("copper"), QStringLiteral("Медь"), 8960.0},
        {QStringLiteral("abs"), QStringLiteral("ABS-пластик"), 1040.0},
        {QStringLiteral("pla"), QStringLiteral("PLA-пластик"), 1240.0}
    };
    return materials;
}

double materialDensityKgPerM3(const QString& materialId)
{
    for (const CadMaterial& material : cadMaterials()) {
        if (material.id == materialId)
            return material.densityKgPerM3;
    }
    return 0.0;
}

int primitiveParameterCount(const QString& primitiveType)
{
    if (primitiveType == QStringLiteral("box") || primitiveType == QStringLiteral("cone"))
        return 3;
    if (primitiveType == QStringLiteral("cylinder") || primitiveType == QStringLiteral("torus") ||
        primitiveType == QStringLiteral("sketch-rectangle") ||
        primitiveType == QStringLiteral("linear-pattern-x") ||
        primitiveType == QStringLiteral("linear-pattern-y") ||
        primitiveType == QStringLiteral("linear-pattern-z"))
        return 2;
    if (primitiveType == QStringLiteral("sphere") || primitiveType == QStringLiteral("sketch-circle") ||
        primitiveType == QStringLiteral("extrude") || primitiveType == QStringLiteral("fillet") ||
        primitiveType == QStringLiteral("chamfer"))
        return 1;
    return 0;
}

QStringList primitiveParameterNames(const QString& primitiveType)
{
    if (primitiveType == QStringLiteral("box"))
        return {QStringLiteral("Длина X"), QStringLiteral("Ширина Y"), QStringLiteral("Высота Z")};
    if (primitiveType == QStringLiteral("cylinder"))
        return {QStringLiteral("Радиус"), QStringLiteral("Высота")};
    if (primitiveType == QStringLiteral("sphere"))
        return {QStringLiteral("Радиус")};
    if (primitiveType == QStringLiteral("cone"))
        return {QStringLiteral("Радиус основания"), QStringLiteral("Радиус вершины"), QStringLiteral("Высота")};
    if (primitiveType == QStringLiteral("torus"))
        return {QStringLiteral("Большой радиус"), QStringLiteral("Малый радиус")};
    if (primitiveType == QStringLiteral("sketch-rectangle"))
        return {QStringLiteral("Ширина"), QStringLiteral("Высота")};
    if (primitiveType == QStringLiteral("sketch-circle"))
        return {QStringLiteral("Радиус")};
    if (primitiveType == QStringLiteral("extrude"))
        return {QStringLiteral("Длина выдавливания")};
    if (primitiveType == QStringLiteral("fillet"))
        return {QStringLiteral("Радиус скругления · мм")};
    if (primitiveType == QStringLiteral("chamfer"))
        return {QStringLiteral("Размер фаски · мм")};
    if (primitiveType == QStringLiteral("linear-pattern-x") ||
        primitiveType == QStringLiteral("linear-pattern-y") ||
        primitiveType == QStringLiteral("linear-pattern-z"))
        return {QStringLiteral("Количество экземпляров"), QStringLiteral("Шаг · мм")};
    return {};
}

void CadObject::setPrimitiveParameters(const std::vector<double>& parameters)
{
    const TopoDS_Shape updatedShape = makePrimitiveShape(primitiveType, parameters, sketchPlane, sketchOrigin);
    shape = updatedShape;
    primitiveParameters = parameters;
}

QMatrix4x4 CadObject::transformMatrix() const
{
    QMatrix4x4 matrix;
    matrix.translate(position);
    matrix.rotate(rotation.x(), 1.0F, 0.0F, 0.0F);
    matrix.rotate(rotation.y(), 0.0F, 1.0F, 0.0F);
    matrix.rotate(rotation.z(), 0.0F, 0.0F, 1.0F);
    matrix.scale(scale);
    return matrix;
}

TopoDS_Shape CadObject::worldShape() const
{
    if (shape.IsNull())
        return {};

    const QMatrix4x4 matrix = transformMatrix();
    const gp_Mat linear(matrix(0, 0), matrix(0, 1), matrix(0, 2),
                        matrix(1, 0), matrix(1, 1), matrix(1, 2),
                        matrix(2, 0), matrix(2, 1), matrix(2, 2));
    gp_GTrsf transform;
    transform.SetVectorialPart(linear);
    transform.SetTranslationPart(gp_XYZ(matrix(0, 3), matrix(1, 3), matrix(2, 3)));
    BRepBuilderAPI_GTransform operation(shape, transform, true);
    if (!operation.IsDone())
        throw CadError(QStringLiteral("Не удалось применить трансформацию к объекту."));
    return operation.Shape();
}

CadObject CadIO::importFile(const QString& filePath)
{
    const QString extension = extensionOf(filePath);
    TopoDS_Shape shape;
    if (extension == QStringLiteral("step") || extension == QStringLiteral("stp"))
        shape = readStep(filePath);
    else if (extension == QStringLiteral("iges") || extension == QStringLiteral("igs"))
        shape = readIges(filePath);
    else if (extension == QStringLiteral("brep") || extension == QStringLiteral("brp"))
        shape = readBrep(filePath);
    else if (extension == QStringLiteral("stl"))
        shape = readStl(filePath);
    else if (extension == QStringLiteral("obj"))
        shape = readObj(filePath);
    else if (extension == QStringLiteral("sldprt") || extension == QStringLiteral("sldasm") ||
             extension == QStringLiteral("slddrw") || extension == QStringLiteral("tfc") ||
             extension == QStringLiteral("grb")) {
        throw CadError(QStringLiteral("Нативный формат не поддерживается. Экспортируйте модель из SolidWorks или T-FLEX в STEP AP214/AP242 либо IGES."));
    } else {
        throw CadError(QStringLiteral("Формат .%1 не поддерживается. Используйте STEP, IGES, BREP, STL или OBJ.").arg(extension));
    }

    CadObject object;
    object.id = cadNewId();
    object.name = QFileInfo(filePath).completeBaseName();
    object.featureType = QStringLiteral("imported-body");
    object.shape = shape;
    return object;
}

CadGeometryProperties CadIO::analyzeShape(const TopoDS_Shape& shape)
{
    CadGeometryProperties result;
    if (shape.IsNull())
        return result;

    GProp_GProps surfaceProperties;
    BRepGProp::SurfaceProperties(shape, surfaceProperties);
    result.surfaceAreaMm2 = std::abs(surfaceProperties.Mass());

    result.volumeMm3 = positiveVolumeOfSolids(shape);
    result.hasVolume = std::isfinite(result.volumeMm3) && result.volumeMm3 > 1.0e-9;

    Bnd_Box bounds;
    BRepBndLib::Add(shape, bounds);
    if (!bounds.IsVoid()) {
        Standard_Real xmin = 0.0, ymin = 0.0, zmin = 0.0;
        Standard_Real xmax = 0.0, ymax = 0.0, zmax = 0.0;
        bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        result.boundsMm = QVector3D(static_cast<float>(xmax - xmin),
                                    static_cast<float>(ymax - ymin),
                                    static_cast<float>(zmax - zmin));
        result.hasBounds = true;
    }
    return result;
}

TopoDS_Shape CadIO::makeExtrusion(const CadObject& sketch, double lengthMm, bool symmetric)
{
    if (sketch.featureType != QStringLiteral("sketch") || sketch.shape.IsNull())
        throw CadError(QStringLiteral("Выдавливание требует существующий эскиз."));
    if (!std::isfinite(lengthMm) || lengthMm <= 1.0e-9)
        throw CadError(QStringLiteral("Длина выдавливания должна быть положительным конечным числом."));
    if (sketch.shape.ShapeType() != TopAbs_WIRE)
        throw CadError(QStringLiteral("Эскиз должен содержать замкнутый профиль."));

    const TopoDS_Wire wire = TopoDS::Wire(sketch.shape);
    BRepBuilderAPI_MakeFace faceBuilder(wire);
    if (!faceBuilder.IsDone() || faceBuilder.Face().IsNull())
        throw CadError(QStringLiteral("Профиль эскиза не замкнут или не является плоским."));
    TopoDS_Shape base = faceBuilder.Face();
    GProp_GProps measuredArea;
    BRepGProp::SurfaceProperties(base, measuredArea);
    if (!std::isfinite(measuredArea.Mass()) || std::abs(measuredArea.Mass()) <= 1.0e-12)
        throw CadError(QStringLiteral("Профиль эскиза имеет нулевую площадь."));
    if (!BRepCheck_Analyzer(base).IsValid())
        throw CadError(QStringLiteral("Профиль эскиза содержит некорректную геометрию."));

    const QVector3D normal = cadSketchPlaneNormal(sketch.sketchPlane);
    gp_Vec direction(normal.x() * lengthMm, normal.y() * lengthMm, normal.z() * lengthMm);
    if (symmetric) {
        gp_Trsf translation;
        translation.SetTranslation(gp_Vec(-direction.X() * 0.5, -direction.Y() * 0.5, -direction.Z() * 0.5));
        BRepBuilderAPI_Transform shifted(base, translation, true);
        if (!shifted.IsDone())
            throw CadError(QStringLiteral("Не удалось подготовить симметричное выдавливание."));
        base = shifted.Shape();
    }

    BRepPrimAPI_MakePrism prism(base, direction, true, true);
    if (!prism.IsDone() || prism.Shape().IsNull())
        throw CadError(QStringLiteral("Open CASCADE не смог построить выдавленный объём."));
    const TopoDS_Shape result = prism.Shape();
    if (!BRepCheck_Analyzer(result).IsValid())
        throw CadError(QStringLiteral("Выдавливание создало некорректную B-Rep-геометрию."));

    GProp_GProps volumeProperties;
    BRepGProp::VolumeProperties(result, volumeProperties, true);
    if (!std::isfinite(volumeProperties.Mass()) || std::abs(volumeProperties.Mass()) <= 1.0e-9)
        throw CadError(QStringLiteral("Эскиз не образует объём при выдавливании."));
    return result;
}

namespace {

void validateVolumeShape(const TopoDS_Shape& shape, const QString& operationName)
{
    if (shape.IsNull())
        throw CadError(QStringLiteral("%1 не создала геометрию.").arg(operationName));
    if (!BRepCheck_Analyzer(shape).IsValid())
        throw CadError(QStringLiteral("%1 создала некорректную B-Rep-геометрию.").arg(operationName));

    const double volume = positiveVolumeOfSolids(shape);
    if (!std::isfinite(volume) || volume <= 1.0e-9)
        throw CadError(QStringLiteral("%1 должна создавать тело с ненулевым объёмом.").arg(operationName));
}

TopoDS_Shape transformedCopy(const TopoDS_Shape& shape, const gp_Trsf& transform, const QString& operationName)
{
    BRepBuilderAPI_Transform operation(shape, transform, true);
    if (!operation.IsDone() || operation.Shape().IsNull())
        throw CadError(QStringLiteral("%1: не удалось применить преобразование.").arg(operationName));
    return operation.Shape();
}

} // namespace

TopoDS_Shape CadIO::makeBoolean(const TopoDS_Shape& left,
                                const TopoDS_Shape& right,
                                const QString& operation)
{
    validateVolumeShape(left, QStringLiteral("Основание булевой операции"));
    validateVolumeShape(right, QStringLiteral("Инструмент булевой операции"));

    TopoDS_Shape result;
    if (operation == QStringLiteral("union")) {
        BRepAlgoAPI_Fuse builder(left, right);
        builder.Build();
        if (!builder.IsDone())
            throw CadError(QStringLiteral("Объединение тел не выполнено. Проверьте пересечение и корректность тел."));
        result = builder.Shape();
    } else if (operation == QStringLiteral("cut")) {
        BRepAlgoAPI_Cut builder(left, right);
        builder.Build();
        if (!builder.IsDone())
            throw CadError(QStringLiteral("Вычитание тел не выполнено. Проверьте корректность тел."));
        result = builder.Shape();
    } else if (operation == QStringLiteral("common")) {
        BRepAlgoAPI_Common builder(left, right);
        builder.Build();
        if (!builder.IsDone())
            throw CadError(QStringLiteral("Пересечение тел не выполнено. Проверьте корректность тел."));
        result = builder.Shape();
    } else {
        throw CadError(QStringLiteral("Неизвестный тип булевой операции «%1».").arg(operation));
    }

    validateVolumeShape(result, QStringLiteral("Булева операция"));
    return result;
}

TopoDS_Shape CadIO::makeFillet(const TopoDS_Shape& shape, double radiusMm)
{
    validateVolumeShape(shape, QStringLiteral("Исходное тело для скругления"));
    if (!std::isfinite(radiusMm) || radiusMm <= 1.0e-9)
        throw CadError(QStringLiteral("Радиус скругления должен быть положительным конечным числом."));

    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    if (edges.Extent() == 0)
        throw CadError(QStringLiteral("В исходном теле не найдены рёбра для скругления."));

    BRepFilletAPI_MakeFillet builder(shape);
    for (Standard_Integer i = 1; i <= edges.Extent(); ++i)
        builder.Add(radiusMm, TopoDS::Edge(edges.FindKey(i)));
    builder.Build();
    if (!builder.IsDone())
        throw CadError(QStringLiteral("Скругление всех рёбер не выполнено. Уменьшите радиус или проверьте топологию тела."));

    const TopoDS_Shape result = builder.Shape();
    validateVolumeShape(result, QStringLiteral("Скругление"));
    return result;
}

TopoDS_Shape CadIO::makeChamfer(const TopoDS_Shape& shape, double distanceMm)
{
    validateVolumeShape(shape, QStringLiteral("Исходное тело для фаски"));
    if (!std::isfinite(distanceMm) || distanceMm <= 1.0e-9)
        throw CadError(QStringLiteral("Размер фаски должен быть положительным конечным числом."));

    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    if (edges.Extent() == 0)
        throw CadError(QStringLiteral("В исходном теле не найдены рёбра для фаски."));

    BRepFilletAPI_MakeChamfer builder(shape);
    for (Standard_Integer i = 1; i <= edges.Extent(); ++i)
        builder.Add(distanceMm, TopoDS::Edge(edges.FindKey(i)));
    builder.Build();
    if (!builder.IsDone())
        throw CadError(QStringLiteral("Фаска на всех рёбрах не построена. Уменьшите размер или проверьте топологию тела."));

    const TopoDS_Shape result = builder.Shape();
    validateVolumeShape(result, QStringLiteral("Фаска"));
    return result;
}

TopoDS_Shape CadIO::makeLinearPattern(const TopoDS_Shape& shape,
                                      const QString& axis,
                                      int instanceCount,
                                      double spacingMm)
{
    validateVolumeShape(shape, QStringLiteral("Исходное тело для линейного массива"));
    if (instanceCount < 2 || instanceCount > 100)
        throw CadError(QStringLiteral("Количество экземпляров линейного массива должно быть от 2 до 100."));
    if (!std::isfinite(spacingMm) || spacingMm <= 1.0e-9)
        throw CadError(QStringLiteral("Шаг линейного массива должен быть положительным конечным числом."));

    const QString normalizedAxis = axis.trimmed().toUpper();
    gp_Vec direction(1.0, 0.0, 0.0);
    if (normalizedAxis == QStringLiteral("Y"))
        direction = gp_Vec(0.0, 1.0, 0.0);
    else if (normalizedAxis == QStringLiteral("Z"))
        direction = gp_Vec(0.0, 0.0, 1.0);
    else if (normalizedAxis != QStringLiteral("X"))
        throw CadError(QStringLiteral("Ось линейного массива должна быть X, Y или Z."));

    TopoDS_Compound compound;
    BRep_Builder compoundBuilder;
    compoundBuilder.MakeCompound(compound);
    compoundBuilder.Add(compound, shape);
    for (int instance = 1; instance < instanceCount; ++instance) {
        gp_Trsf translation;
        translation.SetTranslation(direction * (spacingMm * static_cast<double>(instance)));
        compoundBuilder.Add(compound,
                            transformedCopy(shape, translation, QStringLiteral("Линейный массив")));
    }

    validateVolumeShape(compound, QStringLiteral("Линейный массив"));
    return compound;
}

TopoDS_Shape CadIO::makeMirror(const TopoDS_Shape& shape, const QString& plane)
{
    validateVolumeShape(shape, QStringLiteral("Исходное тело для зеркального отражения"));

    const QString normalizedPlane = plane.trimmed().toUpper();
    gp_Dir normal(0.0, 0.0, 1.0);
    if (normalizedPlane == QStringLiteral("YZ"))
        normal = gp_Dir(1.0, 0.0, 0.0);
    else if (normalizedPlane == QStringLiteral("ZX"))
        normal = gp_Dir(0.0, 1.0, 0.0);
    else if (normalizedPlane != QStringLiteral("XY"))
        throw CadError(QStringLiteral("Плоскость зеркального отражения должна быть XY, YZ или ZX."));

    gp_Trsf reflection;
    reflection.SetMirror(gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), normal));
    const TopoDS_Shape reflected = transformedCopy(shape, reflection, QStringLiteral("Зеркальное отражение"));

    TopoDS_Compound compound;
    BRep_Builder compoundBuilder;
    compoundBuilder.MakeCompound(compound);
    compoundBuilder.Add(compound, shape);
    compoundBuilder.Add(compound, reflected);
    validateVolumeShape(compound, QStringLiteral("Зеркальное отражение"));
    return compound;
}

void CadIO::recomputeParametricFeatures(CadDocument& document)
{
    CadDocument updated = document;
    const ExpressionParser::Values variableValues = ExpressionParser::evaluateVariables(updated.variables);
    QMap<QString, int> indexById;
    for (std::size_t i = 0; i < updated.objects.size(); ++i) {
        CadObject& object = updated.objects[i];
        if (object.id.trimmed().isEmpty())
            object.id = cadNewId();
        if (indexById.contains(object.id))
            throw CadError(QStringLiteral("В дереве построения обнаружен повторяющийся ID фичи."));
        indexById.insert(object.id, static_cast<int>(i));
    }

    std::vector<int> states(updated.objects.size(), 0);
    std::function<void(int)> recompute = [&](int index) {
        int& state = states[static_cast<std::size_t>(index)];
        if (state == 2)
            return;
        if (state == 1)
            throw CadError(QStringLiteral("В графе построения обнаружен цикл."));
        state = 1;

        CadObject& object = updated.objects[static_cast<std::size_t>(index)];
        for (const QString& inputId : object.inputs) {
            const auto input = indexById.constFind(inputId);
            if (input == indexById.cend())
                throw CadError(QStringLiteral("Фича «%1» ссылается на отсутствующий объект.").arg(object.name));
            recompute(input.value());
        }

        if (object.featureType == QStringLiteral("sketch")) {
            if (object.primitiveType != QStringLiteral("sketch-rectangle") &&
                object.primitiveType != QStringLiteral("sketch-circle"))
                throw CadError(QStringLiteral("Тип профиля эскиза «%1» не поддерживается.").arg(object.primitiveType));
            const std::vector<double> parameters = evaluatedParameters(object, variableValues);
            object.shape = makePrimitiveShape(object.primitiveType, parameters, object.sketchPlane, object.sketchOrigin);
            object.primitiveParameters = parameters;
        } else if (object.featureType == QStringLiteral("extrude")) {
            if (object.inputs.size() != 1)
                throw CadError(QStringLiteral("Выдавливание должно ссылаться ровно на один эскиз."));
            const int sketchIndex = indexById.value(object.inputs.front(), -1);
            if (sketchIndex < 0 || updated.objects[static_cast<std::size_t>(sketchIndex)].featureType != QStringLiteral("sketch"))
                throw CadError(QStringLiteral("Выдавливание ссылается не на эскиз."));
            const std::vector<double> parameters = evaluatedParameters(object, variableValues);
            object.shape = makeExtrusion(updated.objects[static_cast<std::size_t>(sketchIndex)], parameters.front(), object.symmetricExtrude);
            object.primitiveParameters = parameters;
        } else if (object.featureType == QStringLiteral("boolean")) {
            if (object.inputs.size() != 2)
                throw CadError(QStringLiteral("Булева операция должна ссылаться ровно на два тела."));
            if (object.inputs[0] == object.inputs[1])
                throw CadError(QStringLiteral("Для булевой операции выбраны два одинаковых тела."));
            const CadObject& left = updated.objects[static_cast<std::size_t>(indexById.value(object.inputs[0]))];
            const CadObject& right = updated.objects[static_cast<std::size_t>(indexById.value(object.inputs[1]))];
            object.shape = makeBoolean(left.worldShape(), right.worldShape(), object.primitiveType);
            object.primitiveParameters.clear();
        } else if (object.featureType == QStringLiteral("fillet") ||
                   object.featureType == QStringLiteral("chamfer")) {
            if (object.primitiveType != object.featureType)
                throw CadError(QStringLiteral("Тип параметров не совпадает с фичей «%1».").arg(object.featureType));
            if (object.inputs.size() != 1)
                throw CadError(QStringLiteral("Фича скругления или фаски должна ссылаться ровно на одно тело."));
            const CadObject& input = updated.objects[static_cast<std::size_t>(indexById.value(object.inputs.front()))];
            const std::vector<double> parameters = evaluatedParameters(object, variableValues);
            object.shape = object.featureType == QStringLiteral("fillet")
                ? makeFillet(input.worldShape(), parameters.front())
                : makeChamfer(input.worldShape(), parameters.front());
            object.primitiveParameters = parameters;
        } else if (object.featureType == QStringLiteral("pattern")) {
            if (object.inputs.size() != 1)
                throw CadError(QStringLiteral("Массив должен ссылаться ровно на одно тело."));
            const CadObject& input = updated.objects[static_cast<std::size_t>(indexById.value(object.inputs.front()))];
            const std::vector<double> parameters = evaluatedParameters(object, variableValues);
            if (parameters.size() != 2 || parameters[0] < 2.0 || parameters[0] > 100.0 ||
                std::floor(parameters[0]) != parameters[0])
                throw CadError(QStringLiteral("Количество экземпляров массива должно быть целым числом от 2 до 100."));
            if (!object.primitiveType.startsWith(QStringLiteral("linear-pattern-")))
                throw CadError(QStringLiteral("Тип массива «%1» не поддерживается.").arg(object.primitiveType));
            const QString axis = object.primitiveType.right(1);
            object.shape = makeLinearPattern(input.worldShape(), axis, static_cast<int>(parameters[0]), parameters[1]);
            object.primitiveParameters = parameters;
        } else if (object.featureType == QStringLiteral("mirror")) {
            if (object.inputs.size() != 1)
                throw CadError(QStringLiteral("Зеркальное отражение должно ссылаться ровно на одно тело."));
            const CadObject& input = updated.objects[static_cast<std::size_t>(indexById.value(object.inputs.front()))];
            if (!object.primitiveType.startsWith(QStringLiteral("mirror-")))
                throw CadError(QStringLiteral("Плоскость зеркального отражения не задана."));
            const QString plane = object.primitiveType.mid(QStringLiteral("mirror-").size());
            object.shape = makeMirror(input.worldShape(), plane);
            object.primitiveParameters.clear();
        } else if (object.featureType == QStringLiteral("primitive")) {
            const std::vector<double> parameters = evaluatedParameters(object, variableValues);
            object.shape = makePrimitiveShape(object.primitiveType, parameters, object.sketchPlane, object.sketchOrigin);
            object.primitiveParameters = parameters;
        } else if (object.featureType == QStringLiteral("imported-body")) {
            if (object.shape.IsNull())
                throw CadError(QStringLiteral("Импортированная фича «%1» не содержит геометрию.").arg(object.name));
        } else {
            throw CadError(QStringLiteral("Тип фичи «%1» не поддерживается.").arg(object.featureType));
        }
        state = 2;
    };

    for (std::size_t i = 0; i < updated.objects.size(); ++i)
        recompute(static_cast<int>(i));
    document = std::move(updated);
}

std::vector<CadTriangle> CadIO::triangulate(const TopoDS_Shape& shape,
                                             std::size_t objectIndex,
                                             double linearDeflection)
{
    std::vector<CadTriangle> result;
    if (shape.IsNull())
        return result;

    BRepMesh_IncrementalMesh mesher(shape, std::max(0.01, linearDeflection), false, 0.35, true);

    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face face = TopoDS::Face(explorer.Current());
        TopLoc_Location location;
        const auto triangulation = BRep_Tool::Triangulation(face, location);
        if (triangulation.IsNull())
            continue;
        const gp_Trsf localTransform = location.Transformation();

        for (Standard_Integer i = 1; i <= triangulation->NbTriangles(); ++i) {
            Standard_Integer ia = 0, ib = 0, ic = 0;
            triangulation->Triangle(i).Get(ia, ib, ic);
            if (face.Orientation() == TopAbs_REVERSED)
                std::swap(ib, ic);

            gp_Pnt pa = triangulation->Node(ia);
            gp_Pnt pb = triangulation->Node(ib);
            gp_Pnt pc = triangulation->Node(ic);
            pa.Transform(localTransform);
            pb.Transform(localTransform);
            pc.Transform(localTransform);

            const QVector3D a(static_cast<float>(pa.X()), static_cast<float>(pa.Y()), static_cast<float>(pa.Z()));
            const QVector3D b(static_cast<float>(pb.X()), static_cast<float>(pb.Y()), static_cast<float>(pb.Z()));
            const QVector3D c(static_cast<float>(pc.X()), static_cast<float>(pc.Y()), static_cast<float>(pc.Z()));
            QVector3D normal = QVector3D::crossProduct(b - a, c - a);
            if (normal.lengthSquared() < 1.0e-12F)
                continue;
            normal.normalize();
            result.push_back({a, b, c, normal, objectIndex});
        }
    }
    return result;
}

CadDocument CadIO::loadProject(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        throw CadError(QStringLiteral("Не удалось открыть проект."));
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject())
        throw CadError(QStringLiteral("Файл проекта не является корректным JSON."));

    const QJsonObject root = json.object();
    const int projectVersion = root.value(QStringLiteral("version")).toInt();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("aoscad-studio-project") ||
        (projectVersion != 2 && projectVersion != 3 && projectVersion != 4)) {
        if (root.contains(QStringLiteral("scene")))
            throw CadError(QStringLiteral("Это проект старой Web/Electron-версии AoSCAD. Экспортируйте в ней геометрию в STL или OBJ и импортируйте сетку в C++-версию."));
        throw CadError(QStringLiteral("Формат проекта AoSCAD не поддерживается."));
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
        throw CadError(QStringLiteral("Не удалось создать временную папку для открытия проекта."));

    CadDocument document;
    document.name = root.value(QStringLiteral("name")).toString(QStringLiteral("Безымянный проект"));
    document.units = root.value(QStringLiteral("units")).toString(QStringLiteral("мм"));
    if (projectVersion >= 4) {
        const QJsonObject variableItems = root.value(QStringLiteral("variables")).toObject();
        for (auto it = variableItems.constBegin(); it != variableItems.constEnd(); ++it) {
            if (!it.value().isString())
                throw CadError(QStringLiteral("Таблица переменных проекта повреждена."));
            document.variables.insert(it.key(), it.value().toString());
        }
    }
    const QJsonArray objects = root.value(QStringLiteral("objects")).toArray();
    document.objects.reserve(static_cast<std::size_t>(objects.size()));

    for (int i = 0; i < objects.size(); ++i) {
        const QJsonObject item = objects[i].toObject();
        CadObject object;
        object.id = projectVersion >= 4 ? item.value(QStringLiteral("id")).toString() : cadNewId();
        if (object.id.trimmed().isEmpty())
            object.id = cadNewId();
        object.name = item.value(QStringLiteral("name")).toString(QStringLiteral("Объект %1").arg(i + 1));
        object.color = QColor(item.value(QStringLiteral("color")).toString(QStringLiteral("#6997A4")));
        if (!object.color.isValid())
            object.color = QColor(105, 151, 164);
        object.visible = item.value(QStringLiteral("visible")).toBool(true);
        object.position = vectorFromJson(item.value(QStringLiteral("position")), {});
        object.rotation = vectorFromJson(item.value(QStringLiteral("rotation")), {});
        object.scale = vectorFromJson(item.value(QStringLiteral("scale")), QVector3D(1, 1, 1));
        if (projectVersion >= 3) {
            const QString primitiveType = item.value(QStringLiteral("primitiveType")).toString();
            const QJsonArray parameterValues = item.value(QStringLiteral("primitiveParameters")).toArray();
            const int expectedParameterCount = primitiveParameterCount(primitiveType);
            if (expectedParameterCount > 0 && expectedParameterCount == parameterValues.size()) {
                object.primitiveType = primitiveType;
                object.primitiveParameters.reserve(static_cast<std::size_t>(parameterValues.size()));
                for (const QJsonValue& parameter : parameterValues) {
                    if (!parameter.isDouble()) {
                        object.primitiveType.clear();
                        object.primitiveParameters.clear();
                        break;
                    }
                    object.primitiveParameters.push_back(parameter.toDouble());
                }
            } else if (parameterValues.isEmpty() &&
                       (primitiveType == QStringLiteral("union") || primitiveType == QStringLiteral("cut") ||
                        primitiveType == QStringLiteral("common") || primitiveType == QStringLiteral("mirror-xy") ||
                        primitiveType == QStringLiteral("mirror-yz") || primitiveType == QStringLiteral("mirror-zx"))) {
                object.primitiveType = primitiveType;
            }
            const QString materialId = item.value(QStringLiteral("materialId")).toString(QStringLiteral("unassigned"));
            object.materialId = materialId;
            const bool knownMaterial = std::any_of(cadMaterials().begin(), cadMaterials().end(), [&object](const CadMaterial& material) {
                return material.id == object.materialId;
            });
            if (!knownMaterial)
                object.materialId = QStringLiteral("unassigned");
        }
        if (projectVersion >= 4) {
            object.featureType = item.value(QStringLiteral("featureType")).toString(
                object.primitiveType.isEmpty() ? QStringLiteral("imported-body") : QStringLiteral("primitive"));
            object.suppressed = item.value(QStringLiteral("suppressed")).toBool(false);
            object.sketchPlane = item.value(QStringLiteral("sketchPlane")).toString(QStringLiteral("XY"));
            object.sketchOrigin = QVector2D(
                static_cast<float>(item.value(QStringLiteral("sketchOriginX")).toDouble()),
                static_cast<float>(item.value(QStringLiteral("sketchOriginY")).toDouble()));
            const QJsonArray constraints = item.value(QStringLiteral("sketchConstraints")).toArray();
            for (const QJsonValue& constraint : constraints) {
                if (!constraint.isString())
                    throw CadError(QStringLiteral("Ограничения эскиза в проекте повреждены."));
                object.sketchConstraints.push_back(constraint.toString());
            }
            object.symmetricExtrude = item.value(QStringLiteral("symmetricExtrude")).toBool(false);
            const QJsonArray inputIds = item.value(QStringLiteral("inputs")).toArray();
            for (const QJsonValue& input : inputIds) {
                if (!input.isString())
                    throw CadError(QStringLiteral("Ссылки графа построения повреждены."));
                object.inputs.push_back(input.toString());
            }
            const QJsonObject expressions = item.value(QStringLiteral("parameterExpressions")).toObject();
            for (auto it = expressions.constBegin(); it != expressions.constEnd(); ++it) {
                if (!it.value().isString())
                    throw CadError(QStringLiteral("Формулы параметров проекта повреждены."));
                object.parameterExpressions.insert(it.key(), it.value().toString());
            }
        } else {
            object.featureType = object.primitiveType.isEmpty()
                ? QStringLiteral("imported-body")
                : QStringLiteral("primitive");
        }
        const QByteArray shapeData = item.value(QStringLiteral("brep")).toString().toLatin1();
        object.shape = shapeFromBase64(shapeData, temporaryDirectory, static_cast<std::size_t>(i));
        document.objects.push_back(std::move(object));
    }
    recomputeParametricFeatures(document);
    return document;
}

void CadIO::saveProject(const CadDocument& document, const QString& filePath)
{
    CadDocument serializable = document;
    recomputeParametricFeatures(serializable);

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
        throw CadError(QStringLiteral("Не удалось создать временную папку для сохранения проекта."));

    QJsonObject variables;
    for (auto it = serializable.variables.cbegin(); it != serializable.variables.cend(); ++it)
        variables.insert(it.key(), it.value());

    QJsonArray objects;
    for (std::size_t i = 0; i < serializable.objects.size(); ++i) {
        const CadObject& object = serializable.objects[i];
        QJsonObject item;
        item.insert(QStringLiteral("id"), object.id);
        item.insert(QStringLiteral("name"), object.name);
        item.insert(QStringLiteral("featureType"), object.featureType);
        item.insert(QStringLiteral("inputs"), QJsonArray::fromStringList(object.inputs));
        item.insert(QStringLiteral("suppressed"), object.suppressed);
        item.insert(QStringLiteral("color"), object.color.name(QColor::HexArgb));
        item.insert(QStringLiteral("visible"), object.visible);
        item.insert(QStringLiteral("position"), vectorToJson(object.position));
        item.insert(QStringLiteral("rotation"), vectorToJson(object.rotation));
        item.insert(QStringLiteral("scale"), vectorToJson(object.scale));
        item.insert(QStringLiteral("primitiveType"), object.primitiveType);
        QJsonArray primitiveParameters;
        for (double parameter : object.primitiveParameters)
            primitiveParameters.push_back(parameter);
        item.insert(QStringLiteral("primitiveParameters"), primitiveParameters);
        QJsonObject parameterExpressions;
        for (auto it = object.parameterExpressions.cbegin(); it != object.parameterExpressions.cend(); ++it)
            parameterExpressions.insert(it.key(), it.value());
        item.insert(QStringLiteral("parameterExpressions"), parameterExpressions);
        item.insert(QStringLiteral("sketchPlane"), object.sketchPlane);
        item.insert(QStringLiteral("sketchOriginX"), object.sketchOrigin.x());
        item.insert(QStringLiteral("sketchOriginY"), object.sketchOrigin.y());
        QJsonArray sketchConstraints;
        for (const QString& constraint : object.sketchConstraints)
            sketchConstraints.push_back(constraint);
        item.insert(QStringLiteral("sketchConstraints"), sketchConstraints);
        item.insert(QStringLiteral("symmetricExtrude"), object.symmetricExtrude);
        item.insert(QStringLiteral("materialId"), object.materialId);
        item.insert(QStringLiteral("brep"), QString::fromLatin1(shapeToBase64(object.shape, temporaryDirectory, i)));
        objects.push_back(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("aoscad-studio-project"));
    root.insert(QStringLiteral("version"), 4);
    root.insert(QStringLiteral("name"), serializable.name);
    root.insert(QStringLiteral("units"), serializable.units);
    root.insert(QStringLiteral("variables"), variables);
    root.insert(QStringLiteral("objects"), objects);

    const QByteArray projectData = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(projectData) != projectData.size())
        throw CadError(QStringLiteral("Не удалось сохранить проект AoSCAD."));
}

void CadIO::exportFile(const CadDocument& document,
                       const QString& filePath,
                       std::optional<std::size_t> objectIndex)
{
    const QString extension = extensionOf(filePath);
    if (extension == QStringLiteral("aoscad")) {
        saveProject(document, filePath);
        return;
    }
    if (extension == QStringLiteral("obj")) {
        writeObj(document, filePath, objectIndex);
        return;
    }

    const TopoDS_Shape shape = compoundFor(document, objectIndex);
    const QByteArray path = nativePath(filePath);
    if (extension == QStringLiteral("stl")) {
        BRepMesh_IncrementalMesh mesher(shape, 0.2, false, 0.25, true);
        StlAPI_Writer writer;
        writer.ASCIIMode() = false;
        if (!writer.Write(shape, path.constData()))
            throw CadError(QStringLiteral("Не удалось записать STL-файл."));
    } else if (extension == QStringLiteral("step") || extension == QStringLiteral("stp")) {
        STEPControl_Writer writer;
        if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone ||
            writer.Write(path.constData()) != IFSelect_RetDone)
            throw CadError(QStringLiteral("Не удалось экспортировать STEP."));
    } else if (extension == QStringLiteral("iges") || extension == QStringLiteral("igs")) {
        IGESControl_Writer writer("MM", 0);
        if (!writer.AddShape(shape))
            throw CadError(QStringLiteral("Не удалось подготовить геометрию для IGES."));
        writer.ComputeModel();
        if (!writer.Write(path.constData()))
            throw CadError(QStringLiteral("Не удалось экспортировать IGES."));
    } else if (extension == QStringLiteral("brep") || extension == QStringLiteral("brp")) {
        const QString temporaryPath = filePath + QStringLiteral(".tmp");
        const QByteArray temporaryNativePath = nativePath(temporaryPath);
        BRepTools::Write(shape, temporaryNativePath.constData());
        if (!QFileInfo::exists(temporaryPath) || QFileInfo(temporaryPath).size() == 0) {
            QFile::remove(temporaryPath);
            throw CadError(QStringLiteral("Не удалось записать BREP-файл."));
        }
        if (QFileInfo::exists(filePath) && !QFile::remove(filePath)) {
            QFile::remove(temporaryPath);
            throw CadError(QStringLiteral("Не удалось заменить BREP-файл."));
        }
        if (!QFile::rename(temporaryPath, filePath)) {
            QFile::remove(temporaryPath);
            throw CadError(QStringLiteral("Не удалось завершить запись BREP-файла."));
        }
    } else {
        throw CadError(QStringLiteral("Формат .%1 не поддерживается для экспорта.").arg(extension));
    }
}
