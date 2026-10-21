#pragma once

#include "CadModel.hpp"

#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QVector2D>

#include <vector>

class QMouseEvent;
class QWheelEvent;

class CadViewport final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    enum class Tool { Select, Move, Rotate, Scale, Measure };

    explicit CadViewport(CadDocument* document, QWidget* parent = nullptr);
    ~CadViewport() override;

    void setDocument(CadDocument* document);
    void setSelectedObject(int index);
    int selectedObject() const { return selectedObject_; }
    void setTool(Tool tool);
    Tool tool() const { return tool_; }
    void setGridVisible(bool visible);
    void refreshGeometry();
    void fitAll();
    void setStandardView(const QString& viewName);

signals:
    void selectionChanged(int objectIndex);
    void objectChanged(bool committed);
    void measurementCompleted(double distanceMm);
    void worldPositionChanged(const QVector3D& position);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct Vertex {
        float x, y, z;
        float nx, ny, nz;
    };
    struct Batch {
        int objectIndex;
        int firstVertex;
        int vertexCount;
        QColor color;
    };
    struct PickResult {
        int objectIndex{-1};
        QVector3D point;
        float distance{0.0F};
    };

    CadDocument* document_{nullptr};
    QOpenGLShaderProgram shader_;
    QOpenGLVertexArrayObject vertexArray_;
    QOpenGLBuffer vertexBuffer_{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer gridBuffer_{QOpenGLBuffer::VertexBuffer};
    std::vector<Vertex> vertices_;
    std::vector<Vertex> gridVertices_;
    std::vector<Batch> batches_;
    std::vector<CadTriangle> pickTriangles_;
    int selectedObject_{-1};
    Tool tool_{Tool::Select};
    bool gridVisible_{true};
    bool glReady_{false};

    float yawDegrees_{-45.0F};
    float pitchDegrees_{28.0F};
    float cameraDistance_{320.0F};
    QVector3D target_{0.0F, 0.0F, 0.0F};

    Qt::MouseButton navigationButton_{Qt::NoButton};
    bool leftPressed_{false};
    bool objectDrag_{false};
    bool objectChangedDuringDrag_{false};
    QPoint pressPosition_;
    QPoint lastPosition_;
    QVector3D dragStartPosition_;
    QVector3D dragStartRotation_;
    QVector3D dragStartScale_;
    QVector3D dragPlaneStart_;
    QVector3D dragPlanePoint_;
    QVector3D dragPlaneNormal_;
    std::optional<QVector3D> measurementStart_;

    static Vertex makeVertex(const QVector3D& point, const QVector3D& normal);
    QMatrix4x4 projectionMatrix() const;
    QMatrix4x4 viewMatrix() const;
    QVector3D cameraPosition() const;
    bool screenRay(const QPoint& point, QVector3D& origin, QVector3D& direction) const;
    bool planeIntersection(const QPoint& point, float planeZ, QVector3D& result) const;
    bool planeIntersection(const QPoint& point, const QVector3D& planePoint, const QVector3D& planeNormal, QVector3D& result) const;
    PickResult pick(const QPoint& point) const;
    void rebuildGeometry();
    void rebuildGrid();
    void uploadBuffers();
    void beginObjectDrag(const QPoint& point);
    void updateObjectDrag(const QPoint& point);
    void finishObjectDrag();
};
