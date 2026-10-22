#include "CadModel.hpp"

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
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
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Mat.hxx>
#include <gp_Pnt.hxx>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
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
        if (!object.visible || object.shape.IsNull())
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
        if (!object.visible || object.shape.IsNull())
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

} // namespace

CadError::CadError(const QString& message)
    : std::runtime_error(message.toUtf8().constData())
{
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
    object.name = QFileInfo(filePath).completeBaseName();
    object.shape = shape;
    return object;
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
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("aoscad-studio-project") ||
        root.value(QStringLiteral("version")).toInt() != 2) {
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
    const QJsonArray objects = root.value(QStringLiteral("objects")).toArray();
    document.objects.reserve(static_cast<std::size_t>(objects.size()));

    for (int i = 0; i < objects.size(); ++i) {
        const QJsonObject item = objects[i].toObject();
        CadObject object;
        object.name = item.value(QStringLiteral("name")).toString(QStringLiteral("Объект %1").arg(i + 1));
        object.color = QColor(item.value(QStringLiteral("color")).toString(QStringLiteral("#6997A4")));
        if (!object.color.isValid())
            object.color = QColor(105, 151, 164);
        object.visible = item.value(QStringLiteral("visible")).toBool(true);
        object.position = vectorFromJson(item.value(QStringLiteral("position")), {});
        object.rotation = vectorFromJson(item.value(QStringLiteral("rotation")), {});
        object.scale = vectorFromJson(item.value(QStringLiteral("scale")), QVector3D(1, 1, 1));
        const QByteArray shapeData = item.value(QStringLiteral("brep")).toString().toLatin1();
        object.shape = shapeFromBase64(shapeData, temporaryDirectory, static_cast<std::size_t>(i));
        document.objects.push_back(std::move(object));
    }
    return document;
}

void CadIO::saveProject(const CadDocument& document, const QString& filePath)
{
    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
        throw CadError(QStringLiteral("Не удалось создать временную папку для сохранения проекта."));

    QJsonArray objects;
    for (std::size_t i = 0; i < document.objects.size(); ++i) {
        const CadObject& object = document.objects[i];
        QJsonObject item;
        item.insert(QStringLiteral("name"), object.name);
        item.insert(QStringLiteral("color"), object.color.name(QColor::HexArgb));
        item.insert(QStringLiteral("visible"), object.visible);
        item.insert(QStringLiteral("position"), vectorToJson(object.position));
        item.insert(QStringLiteral("rotation"), vectorToJson(object.rotation));
        item.insert(QStringLiteral("scale"), vectorToJson(object.scale));
        item.insert(QStringLiteral("brep"), QString::fromLatin1(shapeToBase64(object.shape, temporaryDirectory, i)));
        objects.push_back(item);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("aoscad-studio-project"));
    root.insert(QStringLiteral("version"), 2);
    root.insert(QStringLiteral("name"), document.name);
    root.insert(QStringLiteral("units"), document.units);
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
