#pragma once

#include <QColor>
#include <QMatrix4x4>
#include <QString>
#include <QVector3D>

#include <TopoDS_Shape.hxx>

#include <optional>
#include <stdexcept>
#include <vector>

struct CadObject {
    QString name;
    TopoDS_Shape shape;
    QColor color{105, 151, 164};
    QVector3D position{0.0F, 0.0F, 0.0F};
    QVector3D rotation{0.0F, 0.0F, 0.0F};
    QVector3D scale{1.0F, 1.0F, 1.0F};
    bool visible{true};

    QMatrix4x4 transformMatrix() const;
    TopoDS_Shape worldShape() const;
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
    static std::vector<CadTriangle> triangulate(const TopoDS_Shape& shape,
                                                 std::size_t objectIndex = 0,
                                                 double linearDeflection = 0.5);
};
