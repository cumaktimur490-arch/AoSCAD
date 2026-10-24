#pragma once

#include <QColor>
#include <QMatrix4x4>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <TopoDS_Shape.hxx>

#include <optional>
#include <stdexcept>
#include <vector>

struct CadMaterial {
    QString id;
    QString name;
    double densityKgPerM3{0.0};
};

struct CadGeometryProperties {
    double surfaceAreaMm2{0.0};
    double volumeMm3{0.0};
    QVector3D boundsMm{};
    bool hasBounds{false};
    bool hasVolume{false};
};

const std::vector<CadMaterial>& cadMaterials();
double materialDensityKgPerM3(const QString& materialId);
int primitiveParameterCount(const QString& primitiveType);
QStringList primitiveParameterNames(const QString& primitiveType);

struct CadObject {
    QString name;
    TopoDS_Shape shape;
    QColor color{105, 151, 164};
    QVector3D position{0.0F, 0.0F, 0.0F};
    QVector3D rotation{0.0F, 0.0F, 0.0F};
    QVector3D scale{1.0F, 1.0F, 1.0F};
    bool visible{true};
    QString primitiveType;
    std::vector<double> primitiveParameters;
    QString materialId{QStringLiteral("unassigned")};

    QMatrix4x4 transformMatrix() const;
    TopoDS_Shape worldShape() const;
    void setPrimitiveParameters(const std::vector<double>& parameters);
};

struct CadDocument {
    QString name{"Безымянный проект"};
    QString units{"мм"};
    std::vector<CadObject> objects;
};

struct CadTriangle {
    QVector3D a;
    QVector3D b;
    QVector3D c;
    QVector3D normal;
    std::size_t objectIndex{0};
};

class CadError : public std::runtime_error {
public:
    explicit CadError(const QString& message);
};

class CadIO {
public:
    static CadObject importFile(const QString& filePath);
    static CadDocument loadProject(const QString& filePath);
    static void saveProject(const CadDocument& document, const QString& filePath);
    static void exportFile(const CadDocument& document,
                           const QString& filePath,
                           std::optional<std::size_t> objectIndex = std::nullopt);
    static CadGeometryProperties analyzeShape(const TopoDS_Shape& shape);
    static std::vector<CadTriangle> triangulate(const TopoDS_Shape& shape,
                                                 std::size_t objectIndex = 0,
                                                 double linearDeflection = 0.5);
};
