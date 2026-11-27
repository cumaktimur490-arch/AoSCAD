#pragma once

#include <QColor>
#include <QMap>
#include <QMatrix4x4>
#include <QString>
#include <QStringList>
#include <QVector2D>
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
    QString id;
    QString name;
    TopoDS_Shape shape;
    QColor color{105, 151, 164};
    QVector3D position{0.0F, 0.0F, 0.0F};
    QVector3D rotation{0.0F, 0.0F, 0.0F};
    QVector3D scale{1.0F, 1.0F, 1.0F};
    bool visible{true};
    bool suppressed{false};
    QString featureType{QStringLiteral("imported-body")};
    QStringList inputs;
    QString primitiveType;
    std::vector<double> primitiveParameters;
    QMap<QString, QString> parameterExpressions;
    QString sketchPlane{QStringLiteral("XY")};
    QVector2D sketchOrigin{};
    QStringList sketchConstraints;
    bool symmetricExtrude{false};
    QString materialId{QStringLiteral("unassigned")};

    QMatrix4x4 transformMatrix() const;
    TopoDS_Shape worldShape() const;
    void setPrimitiveParameters(const std::vector<double>& parameters);
};

struct CadDocument {
    QString name{"Безымянный проект"};
    QString units{"мм"};
    QMap<QString, QString> variables;
    std::vector<CadObject> objects;
};

QString cadNewId();
QVector3D cadSketchPointToWorld(const QString& plane, const QVector2D& point);
QVector2D cadSketchPointFromWorld(const QString& plane, const QVector3D& point);
QVector3D cadSketchPlaneNormal(const QString& plane);

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
    static TopoDS_Shape makeExtrusion(const CadObject& sketch, double lengthMm, bool symmetric = false);
    static TopoDS_Shape makeBoolean(const TopoDS_Shape& left,
                                    const TopoDS_Shape& right,
                                    const QString& operation);
    static TopoDS_Shape makeFillet(const TopoDS_Shape& shape, double radiusMm);
    static TopoDS_Shape makeChamfer(const TopoDS_Shape& shape, double distanceMm);
    static TopoDS_Shape makeLinearPattern(const TopoDS_Shape& shape,
                                          const QString& axis,
                                          int instanceCount,
                                          double spacingMm);
    static TopoDS_Shape makeMirror(const TopoDS_Shape& shape, const QString& plane);
    static void recomputeParametricFeatures(CadDocument& document);
    static std::vector<CadTriangle> triangulate(const TopoDS_Shape& shape,
                                                 std::size_t objectIndex = 0,
                                                 double linearDeflection = 0.5);
};
