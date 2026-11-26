#include "CadViewport.hpp"

#include <Standard_Failure.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <QDebug>
#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLShader>
#include <QPainter>
#include <QPolygonF>
#include <QVector4D>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kFieldOfView = 45.0F;

QVector4D colorVector(const QColor& color)
{
    return QVector4D(color.redF(), color.greenF(), color.blueF(), color.alphaF());
}

bool rayTriangle(const QVector3D& origin,
                 const QVector3D& direction,
                 const CadTriangle& triangle,
                 float& distance)
{
    const QVector3D edge1 = triangle.b - triangle.a;
    const QVector3D edge2 = triangle.c - triangle.a;
    const QVector3D p = QVector3D::crossProduct(direction, edge2);
    const float determinant = QVector3D::dotProduct(edge1, p);
    if (std::abs(determinant) < 1.0e-7F)
        return false;

    const float inverse = 1.0F / determinant;
    const QVector3D t = origin - triangle.a;
    const float u = QVector3D::dotProduct(t, p) * inverse;
    if (u < 0.0F || u > 1.0F)
        return false;
    const QVector3D q = QVector3D::crossProduct(t, edge1);
    const float v = QVector3D::dotProduct(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F)
        return false;
    distance = QVector3D::dotProduct(edge2, q) * inverse;
    return distance >= 0.0F;
}

struct ViewCubeFace {
    QString viewName;
    QString label;
    QPolygonF polygon;
    QColor fillColor;
};

std::array<ViewCubeFace, 3> makeViewCubeFaces(int width, int height)
{
    const QPointF center(std::max(58, width - 72), std::min(76, std::max(64, height - 64)));
    const auto point = [&center](qreal x, qreal y) { return center + QPointF(x, y); };
    std::array<ViewCubeFace, 3> faces;

    faces[0].viewName = QStringLiteral("top");
    faces[0].label = QStringLiteral("ВЕРХ");
    faces[0].polygon << point(0, -56) << point(56, -28) << point(0, 0) << point(-56, -28);
    faces[0].fillColor = QColor(QStringLiteral("#FFFFFF"));

    faces[1].viewName = QStringLiteral("front");
    faces[1].label = QStringLiteral("ПЕРЕД");
    faces[1].polygon << point(-56, -28) << point(0, 0) << point(0, 56) << point(-56, 28);
    faces[1].fillColor = QColor(QStringLiteral("#EEF3F7"));

    faces[2].viewName = QStringLiteral("right");
    faces[2].label = QStringLiteral("ПРАВО");
    faces[2].polygon << point(0, 0) << point(56, -28) << point(56, 28) << point(0, 56);
    faces[2].fillColor = QColor(QStringLiteral("#E5ECF2"));
    return faces;
}

QString planeCode(CadViewport::ReferencePlane plane)
{
    switch (plane) {
    case CadViewport::ReferencePlane::XY: return QStringLiteral("XY");
    case CadViewport::ReferencePlane::YZ: return QStringLiteral("YZ");
    case CadViewport::ReferencePlane::ZX: return QStringLiteral("ZX");
    case CadViewport::ReferencePlane::None: break;
    }
    return QStringLiteral("XY");
}

QVector2D snapSketchPoint(const QVector2D& point)
{
    const auto snap = [](float value) {
        constexpr float spacing = 5.0F;
        float snapped = std::round(value / spacing) * spacing;
        if (std::abs(snapped) < spacing * 0.5F)
            snapped = 0.0F;
        return snapped;
    };
    return QVector2D(snap(point.x()), snap(point.y()));
}
} // namespace

CadViewport::Vertex CadViewport::makeVertex(const QVector3D& point, const QVector3D& normal)
{
    return {point.x(), point.y(), point.z(), normal.x(), normal.y(), normal.z()};
}

CadViewport::CadViewport(CadDocument* document, QWidget* parent)
    : QOpenGLWidget(parent), document_(document)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    setMouseTracking(true);
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
}

CadViewport::~CadViewport()
{
    if (context() && context()->isValid()) {
        makeCurrent();
        vertexArray_.destroy();
        vertexBuffer_.destroy();
        lineBuffer_.destroy();
        gridBuffer_.destroy();
        backgroundBuffer_.destroy();
        doneCurrent();
    }
}

void CadViewport::setDocument(CadDocument* document)
{
    document_ = document;
    selectedObject_ = -1;
    temporarilyHiddenObject_ = -1;
    activeReferencePlane_ = ReferencePlane::None;
    previewShape_.Nullify();
    cancelSketch();
    rebuildGrid();
    rebuildGeometry();
    update();
}

void CadViewport::setSelectedObject(int index)
{
    if (!document_ || index < 0 || static_cast<std::size_t>(index) >= document_->objects.size())
        index = -1;
    const bool clearedPlane = index >= 0 && activeReferencePlane_ != ReferencePlane::None;
    if (index >= 0) {
        cancelSketch();
        activeReferencePlane_ = ReferencePlane::None;
    }
    if (selectedObject_ == index && !clearedPlane)
        return;
    selectedObject_ = index;
    update();
}

void CadViewport::setTool(Tool tool)
{
    cancelSketch();
    tool_ = tool;
    switch (tool_) {
    case Tool::Select: setCursor(Qt::ArrowCursor); break;
    case Tool::Move: setCursor(Qt::SizeAllCursor); break;
    case Tool::Rotate: setCursor(Qt::CrossCursor); break;
    case Tool::Scale: setCursor(Qt::SizeVerCursor); break;
    case Tool::Measure: setCursor(Qt::PointingHandCursor); break;
    }
}

void CadViewport::setGridVisible(bool visible)
{
    gridVisible_ = visible;
    update();
}

void CadViewport::setActiveReferencePlane(ReferencePlane plane)
{
    if (activeReferencePlane_ == plane)
        return;
    cancelSketch();
    activeReferencePlane_ = plane;
    rebuildGrid();
}

void CadViewport::refreshGeometry()
{
    rebuildGeometry();
}

QVector3D CadViewport::cameraPosition() const
{
    const float yaw = qDegreesToRadians(yawDegrees_);
    const float pitch = qDegreesToRadians(pitchDegrees_);
    const float horizontal = std::cos(pitch) * cameraDistance_;
    return target_ + QVector3D(horizontal * std::cos(yaw),
                               horizontal * std::sin(yaw),
                               std::sin(pitch) * cameraDistance_);
}

QMatrix4x4 CadViewport::projectionMatrix() const
{
    QMatrix4x4 projection;
    const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
    projection.perspective(kFieldOfView, std::max(0.01F, aspect), 0.1F, 1000000.0F);
    return projection;
}

QMatrix4x4 CadViewport::viewMatrix() const
{
    QMatrix4x4 view;
    view.lookAt(cameraPosition(), target_, QVector3D(0.0F, 0.0F, 1.0F));
    return view;
}

void CadViewport::setStandardView(const QString& viewName)
{
    if (viewName == QStringLiteral("top")) {
        currentViewName_ = QStringLiteral("top");
        yawDegrees_ = -90.0F;
        pitchDegrees_ = 89.0F;
    } else if (viewName == QStringLiteral("front")) {
        currentViewName_ = QStringLiteral("front");
        yawDegrees_ = -90.0F;
        pitchDegrees_ = 0.0F;
    } else if (viewName == QStringLiteral("right")) {
        currentViewName_ = QStringLiteral("right");
        yawDegrees_ = 0.0F;
        pitchDegrees_ = 0.0F;
    } else {
        currentViewName_ = QStringLiteral("iso");
        yawDegrees_ = -45.0F;
        pitchDegrees_ = 28.0F;
    }
    update();
}

void CadViewport::beginSketch(SketchTool tool, ReferencePlane plane)
{
    if (tool == SketchTool::None || plane == ReferencePlane::None)
        return;
    setActiveReferencePlane(plane);
    sketchTool_ = tool;
    sketchDrawing_ = false;
    if (plane == ReferencePlane::XY)
        setStandardView(QStringLiteral("top"));
    else if (plane == ReferencePlane::YZ)
        setStandardView(QStringLiteral("right"));
    else
        setStandardView(QStringLiteral("front"));
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void CadViewport::cancelSketch()
{
    if (sketchTool_ == SketchTool::None && !sketchDrawing_)
        return;
    sketchTool_ = SketchTool::None;
    sketchDrawing_ = false;
    setCursor(Qt::ArrowCursor);
    update();
}

void CadViewport::setPreviewShape(const TopoDS_Shape& shape)
{
    previewShape_ = shape;
    rebuildGeometry();
}

void CadViewport::clearPreviewShape()
{
    previewShape_.Nullify();
    rebuildGeometry();
}

void CadViewport::setTemporarilyHiddenObject(int index)
{
    if (!document_ || index < 0 || static_cast<std::size_t>(index) >= document_->objects.size())
        index = -1;
    if (temporarilyHiddenObject_ == index)
        return;
    temporarilyHiddenObject_ = index;
    rebuildGeometry();
}

void CadViewport::initializeGL()
{
    initializeOpenGLFunctions();
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);
    glClearColor(0.961F, 0.969F, 0.980F, 1.0F);

    const char* vertexShader = R"(
        #version 330 core
        in vec3 aPosition;
        in vec3 aNormal;
        uniform mat4 uViewProjection;
        uniform bool uBackground;
        out vec3 vNormal;
        out float vBackgroundFactor;
        void main() {
            if (uBackground) {
                gl_Position = vec4(aPosition.xy, 0.0, 1.0);
                vNormal = vec3(0.0, 0.0, 1.0);
                vBackgroundFactor = (1.0 - aPosition.y) * 0.5;
            } else {
                gl_Position = uViewProjection * vec4(aPosition, 1.0);
                vNormal = aNormal;
                vBackgroundFactor = 0.0;
            }
        }
    )";
    const char* fragmentShader = R"(
        #version 330 core
        in vec3 vNormal;
        in float vBackgroundFactor;
        uniform vec4 uColor;
        uniform vec4 uBackgroundTop;
        uniform vec4 uBackgroundBottom;
        uniform bool uLit;
        uniform bool uBackground;
        out vec4 fragColor;
        void main() {
            if (uBackground) {
                fragColor = mix(uBackgroundTop, uBackgroundBottom, clamp(vBackgroundFactor, 0.0, 1.0));
                return;
            }
            float light = 1.0;
            if (uLit) {
                vec3 n = normalize(vNormal);
                light = 0.30 + 0.70 * max(dot(n, normalize(vec3(-0.45, -0.55, 0.82))), 0.0);
            }
            fragColor = vec4(uColor.rgb * light, uColor.a);
        }
    )";
    shader_.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    shader_.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    shader_.bindAttributeLocation("aPosition", 0);
    shader_.bindAttributeLocation("aNormal", 1);
    if (!shader_.link())
        qWarning("AoSCAD viewport shader link failed: %s", qPrintable(shader_.log()));

    vertexArray_.create();
    vertexBuffer_.create();
    lineBuffer_.create();
    gridBuffer_.create();
    backgroundBuffer_.create();
    const std::array<Vertex, 6> backgroundVertices{{
        makeVertex(QVector3D(-1.0F, 1.0F, 0.0F), QVector3D(0, 0, 1)),
        makeVertex(QVector3D(1.0F, 1.0F, 0.0F), QVector3D(0, 0, 1)),
        makeVertex(QVector3D(1.0F, -1.0F, 0.0F), QVector3D(0, 0, 1)),
        makeVertex(QVector3D(-1.0F, 1.0F, 0.0F), QVector3D(0, 0, 1)),
        makeVertex(QVector3D(1.0F, -1.0F, 0.0F), QVector3D(0, 0, 1)),
        makeVertex(QVector3D(-1.0F, -1.0F, 0.0F), QVector3D(0, 0, 1))
    }};
    backgroundBuffer_.bind();
    backgroundBuffer_.allocate(backgroundVertices.data(), static_cast<int>(sizeof(backgroundVertices)));
    backgroundBuffer_.release();
    glReady_ = true;
    rebuildGrid();
    rebuildGeometry();
}

void CadViewport::resizeGL(int, int)
{
    update();
}

void CadViewport::paintGL()
{
    const qreal ratio = devicePixelRatioF();
    glViewport(0, 0, qRound(width() * ratio), qRound(height() * ratio));
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!shader_.isLinked())
        return;

    vertexArray_.bind();
    shader_.bind();
    shader_.setUniformValue("uBackground", true);
    shader_.setUniformValue("uBackgroundTop", colorVector(QColor(QStringLiteral("#D8E0E8"))));
    shader_.setUniformValue("uBackgroundBottom", colorVector(QColor(QStringLiteral("#F5F7FA"))));
    shader_.setUniformValue("uLit", false);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    backgroundBuffer_.bind();
    shader_.enableAttributeArray(0);
    shader_.enableAttributeArray(1);
    shader_.setAttributeBuffer(0, GL_FLOAT, static_cast<int>(offsetof(Vertex, x)), 3, sizeof(Vertex));
    shader_.setAttributeBuffer(1, GL_FLOAT, static_cast<int>(offsetof(Vertex, nx)), 3, sizeof(Vertex));
    glDisable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    backgroundBuffer_.release();
    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);

    QMatrix4x4 viewProjection = projectionMatrix() * viewMatrix();
    shader_.setUniformValue("uBackground", false);
    shader_.setUniformValue("uViewProjection", viewProjection);

    if (gridVisible_ && gridBuffer_.isCreated() && !gridVertices_.empty()) {
        gridBuffer_.bind();
        shader_.setUniformValue("uColor", colorVector(QColor(QStringLiteral("#C0C6CC"))));
        shader_.setUniformValue("uLit", false);
        shader_.enableAttributeArray(0);
        shader_.enableAttributeArray(1);
        shader_.setAttributeBuffer(0, GL_FLOAT, static_cast<int>(offsetof(Vertex, x)), 3, sizeof(Vertex));
        shader_.setAttributeBuffer(1, GL_FLOAT, static_cast<int>(offsetof(Vertex, nx)), 3, sizeof(Vertex));
        glLineWidth(1.0F);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gridVertices_.size()));
        gridBuffer_.release();
    }

    if (vertexBuffer_.isCreated() && !vertices_.empty()) {
        vertexBuffer_.bind();
        shader_.enableAttributeArray(0);
        shader_.enableAttributeArray(1);
        shader_.setAttributeBuffer(0, GL_FLOAT, static_cast<int>(offsetof(Vertex, x)), 3, sizeof(Vertex));
        shader_.setAttributeBuffer(1, GL_FLOAT, static_cast<int>(offsetof(Vertex, nx)), 3, sizeof(Vertex));
        shader_.setUniformValue("uLit", true);
        for (const Batch& batch : batches_) {
            if (batch.objectIndex < 0)
                continue;
            QColor color = batch.color;
            if (batch.objectIndex == selectedObject_)
                color = QColor(140, 184, 217);
            shader_.setUniformValue("uColor", colorVector(color));
            glDrawArrays(GL_TRIANGLES, batch.firstVertex, batch.vertexCount);
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const Batch& batch : batches_) {
            if (batch.objectIndex >= 0)
                continue;
            shader_.setUniformValue("uColor", colorVector(batch.color));
            glDrawArrays(GL_TRIANGLES, batch.firstVertex, batch.vertexCount);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        vertexBuffer_.release();
    }

    if (lineBuffer_.isCreated() && !lineVertices_.empty()) {
        lineBuffer_.bind();
        shader_.enableAttributeArray(0);
        shader_.enableAttributeArray(1);
        shader_.setAttributeBuffer(0, GL_FLOAT, static_cast<int>(offsetof(Vertex, x)), 3, sizeof(Vertex));
        shader_.setAttributeBuffer(1, GL_FLOAT, static_cast<int>(offsetof(Vertex, nx)), 3, sizeof(Vertex));
        shader_.setUniformValue("uLit", false);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(1.5F);
        for (const LineBatch& batch : lineBatches_) {
            QColor color = batch.color;
            if (batch.objectIndex >= 0 && batch.objectIndex == selectedObject_)
                color = QColor(QStringLiteral("#0078D7"));
            shader_.setUniformValue("uColor", colorVector(color));
            glDrawArrays(GL_LINES, batch.firstVertex, batch.vertexCount);
        }
        glDisable(GL_BLEND);
        lineBuffer_.release();
    }
    shader_.disableAttributeArray(0);
    shader_.disableAttributeArray(1);
    shader_.release();
    vertexArray_.release();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont overlayFont = painter.font();
    overlayFont.setPointSizeF(8.0);
    overlayFont.setBold(true);
    painter.setFont(overlayFont);

    const auto projectPoint = [this, &viewProjection](const QVector3D& world, QPointF& screen) {
        const QVector4D clip = viewProjection * QVector4D(world, 1.0F);
        if (clip.w() <= 1.0e-4F)
            return false;
        const float x = clip.x() / clip.w();
        const float y = clip.y() / clip.w();
        screen = QPointF((x + 1.0F) * static_cast<float>(width()) * 0.5F,
                         (1.0F - y) * static_cast<float>(height()) * 0.5F);
        return true;
    };

    struct PlaneOverlay {
        ReferencePlane plane;
        QString label;
        QColor color;
        std::array<QVector3D, 4> corners;
    };
    constexpr float planeHalfSize = 70.0F;
    const std::array<PlaneOverlay, 3> planes{{
        {ReferencePlane::XY, QStringLiteral("XY"), QColor(95, 164, 207, 150),
         {QVector3D(-planeHalfSize, -planeHalfSize, 0.0F), QVector3D(planeHalfSize, -planeHalfSize, 0.0F),
          QVector3D(planeHalfSize, planeHalfSize, 0.0F), QVector3D(-planeHalfSize, planeHalfSize, 0.0F)}},
        {ReferencePlane::YZ, QStringLiteral("YZ"), QColor(104, 197, 145, 150),
         {QVector3D(0.0F, -planeHalfSize, -planeHalfSize), QVector3D(0.0F, planeHalfSize, -planeHalfSize),
          QVector3D(0.0F, planeHalfSize, planeHalfSize), QVector3D(0.0F, -planeHalfSize, planeHalfSize)}},
        {ReferencePlane::ZX, QStringLiteral("ZX"), QColor(221, 165, 94, 150),
         {QVector3D(-planeHalfSize, 0.0F, -planeHalfSize), QVector3D(planeHalfSize, 0.0F, -planeHalfSize),
          QVector3D(planeHalfSize, 0.0F, planeHalfSize), QVector3D(-planeHalfSize, 0.0F, planeHalfSize)}}
    }};
    for (const PlaneOverlay& plane : planes) {
        QPolygonF polygon;
        bool projected = true;
        for (const QVector3D& corner : plane.corners) {
            QPointF screen;
            if (!projectPoint(corner, screen)) {
                projected = false;
                break;
            }
            polygon << screen;
        }
        if (!projected)
            continue;

        const bool active = plane.plane == activeReferencePlane_;
        QColor color = active ? QColor(QStringLiteral("#0078D7")) : plane.color;
        painter.setPen(QPen(color, active ? 2.0 : 1.0, active ? Qt::SolidLine : Qt::DashLine));
        if (active)
            painter.setBrush(QColor(0, 120, 215, 22));
        else
            painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(polygon);
        const QPointF labelPosition = polygon.boundingRect().topLeft() + QPointF(5.0, 14.0);
        painter.setPen(active ? QColor(QStringLiteral("#005A9E")) : QColor(QStringLiteral("#45545E")));
        painter.drawText(labelPosition, plane.label);
    }

    if (sketchTool_ != SketchTool::None && sketchDrawing_) {
        QPolygonF profile;
        QString dimensionText;
        const QString plane = planeCode(activeReferencePlane_);
        if (sketchTool_ == SketchTool::Rectangle) {
            const float left = std::min(sketchStart_.x(), sketchCurrent_.x());
            const float right = std::max(sketchStart_.x(), sketchCurrent_.x());
            const float bottom = std::min(sketchStart_.y(), sketchCurrent_.y());
            const float top = std::max(sketchStart_.y(), sketchCurrent_.y());
            const QVector2D corners[]{QVector2D(left, bottom), QVector2D(right, bottom),
                                      QVector2D(right, top), QVector2D(left, top), QVector2D(left, bottom)};
            for (const QVector2D& corner : corners) {
                QPointF screen;
                if (projectPoint(cadSketchPointToWorld(plane, corner), screen))
                    profile << screen;
            }
            dimensionText = QStringLiteral("%1 × %2 мм").arg(right - left, 0, 'g', 4).arg(top - bottom, 0, 'g', 4);
        } else {
            const float radius = (sketchCurrent_ - sketchStart_).length();
            for (int i = 0; i <= 64; ++i) {
                const float angle = 2.0F * kPi * static_cast<float>(i) / 64.0F;
                const QVector2D point = sketchStart_ + QVector2D(std::cos(angle) * radius, std::sin(angle) * radius);
                QPointF screen;
                if (projectPoint(cadSketchPointToWorld(plane, point), screen))
                    profile << screen;
            }
            dimensionText = QStringLiteral("R %1 мм").arg(radius, 0, 'g', 4);
        }
        if (profile.size() > 1) {
            painter.setPen(QPen(QColor(QStringLiteral("#0078D7")), 2.0, Qt::SolidLine));
            painter.setBrush(QColor(0, 120, 215, 26));
            painter.drawPolygon(profile);
            painter.setPen(QColor(QStringLiteral("#005A9E")));
            painter.drawText(profile.boundingRect().bottomRight() + QPointF(8.0, 16.0), dimensionText);
        }
    }

    const std::array<ViewCubeFace, 3> cubeFaces = makeViewCubeFaces(width(), height());
    QFont cubeFont = painter.font();
    cubeFont.setPointSizeF(7.5);
    painter.setFont(cubeFont);
    for (const ViewCubeFace& face : cubeFaces) {
        const bool active = face.viewName == currentViewName_;
        painter.setPen(QPen(active ? QColor(QStringLiteral("#0078D7")) : QColor(QStringLiteral("#71808C")), active ? 1.8 : 1.3));
        painter.setBrush(active ? QColor(QStringLiteral("#CCE4F7")) : face.fillColor);
        painter.drawPolygon(face.polygon);
        painter.setPen(active ? QColor(QStringLiteral("#005A9E")) : QColor(QStringLiteral("#34424D")));
        painter.drawText(face.polygon.boundingRect().adjusted(4.0, 2.0, -4.0, -2.0), Qt::AlignCenter, face.label);
    }
    painter.setPen(QColor(QStringLiteral("#5F6E78")));
    painter.drawText(QPointF(width() - 118.0, 151.0), QStringLiteral("X   Y   Z"));
}

void CadViewport::rebuildGrid()
{
    gridVertices_.clear();
    constexpr int halfLines = 100;
    constexpr float spacing = 5.0F;
    const float extent = halfLines * spacing;
    for (int i = -halfLines; i <= halfLines; ++i) {
        const float at = static_cast<float>(i) * spacing;
        if (activeReferencePlane_ == ReferencePlane::YZ) {
            gridVertices_.push_back(makeVertex(QVector3D(-0.1F, at, -extent), QVector3D(1, 0, 0)));
            gridVertices_.push_back(makeVertex(QVector3D(-0.1F, at, extent), QVector3D(1, 0, 0)));
            gridVertices_.push_back(makeVertex(QVector3D(-0.1F, -extent, at), QVector3D(1, 0, 0)));
            gridVertices_.push_back(makeVertex(QVector3D(-0.1F, extent, at), QVector3D(1, 0, 0)));
        } else if (activeReferencePlane_ == ReferencePlane::ZX) {
            gridVertices_.push_back(makeVertex(QVector3D(-extent, -0.1F, at), QVector3D(0, 1, 0)));
            gridVertices_.push_back(makeVertex(QVector3D(extent, -0.1F, at), QVector3D(0, 1, 0)));
            gridVertices_.push_back(makeVertex(QVector3D(at, -0.1F, -extent), QVector3D(0, 1, 0)));
            gridVertices_.push_back(makeVertex(QVector3D(at, -0.1F, extent), QVector3D(0, 1, 0)));
        } else {
            gridVertices_.push_back(makeVertex(QVector3D(at, -extent, -0.1F), QVector3D(0, 0, 1)));
            gridVertices_.push_back(makeVertex(QVector3D(at, extent, -0.1F), QVector3D(0, 0, 1)));
            gridVertices_.push_back(makeVertex(QVector3D(-extent, at, -0.1F), QVector3D(0, 0, 1)));
            gridVertices_.push_back(makeVertex(QVector3D(extent, at, -0.1F), QVector3D(0, 0, 1)));
        }
    }
    uploadBuffers();
    update();
}

void CadViewport::rebuildGeometry()
{
    vertices_.clear();
    lineVertices_.clear();
    batches_.clear();
    lineBatches_.clear();
    pickTriangles_.clear();

    const auto appendEdges = [this](const TopoDS_Shape& shape, int objectIndex, const QColor& color) {
        const int firstVertex = static_cast<int>(lineVertices_.size());
        for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More(); explorer.Next()) {
            try {
                const BRepAdaptor_Curve curve(TopoDS::Edge(explorer.Current()));
                const double first = curve.FirstParameter();
                const double last = curve.LastParameter();
                if (!std::isfinite(first) || !std::isfinite(last) || std::abs(last - first) < 1.0e-12)
                    continue;
                int segments = 1;
                if (curve.GetType() == GeomAbs_Circle)
                    segments = 64;
                else if (curve.GetType() != GeomAbs_Line)
                    segments = 32;
                gp_Pnt previous = curve.Value(first);
                for (int segment = 1; segment <= segments; ++segment) {
                    const double parameter = first + (last - first) * static_cast<double>(segment) / segments;
                    const gp_Pnt current = curve.Value(parameter);
                    const QVector3D a(static_cast<float>(previous.X()), static_cast<float>(previous.Y()), static_cast<float>(previous.Z()));
                    const QVector3D b(static_cast<float>(current.X()), static_cast<float>(current.Y()), static_cast<float>(current.Z()));
                    lineVertices_.push_back(makeVertex(a, QVector3D(0, 0, 1)));
                    lineVertices_.push_back(makeVertex(b, QVector3D(0, 0, 1)));
                    previous = current;
                }
            } catch (const Standard_Failure& error) {
                qWarning() << "Could not create CAD edge overlay" << error.GetMessageString();
            }
        }
        const int count = static_cast<int>(lineVertices_.size()) - firstVertex;
        if (count > 0)
            lineBatches_.push_back({objectIndex, firstVertex, count, color});
    };

    if (document_) {
        for (std::size_t index = 0; index < document_->objects.size(); ++index) {
            const CadObject& object = document_->objects[index];
            if (static_cast<int>(index) == temporarilyHiddenObject_ || !object.visible ||
                object.suppressed || object.shape.IsNull())
                continue;
            try {
                const TopoDS_Shape worldShape = object.worldShape();
                if (object.featureType == QStringLiteral("sketch")) {
                    appendEdges(worldShape, static_cast<int>(index), object.color);
                    continue;
                }
                const std::vector<CadTriangle> triangles = CadIO::triangulate(worldShape, index);
                if (triangles.empty())
                    continue;

                Batch batch{static_cast<int>(index), static_cast<int>(vertices_.size()), 0, object.color};
                for (const CadTriangle& triangle : triangles) {
                    vertices_.push_back(makeVertex(triangle.a, triangle.normal));
                    vertices_.push_back(makeVertex(triangle.b, triangle.normal));
                    vertices_.push_back(makeVertex(triangle.c, triangle.normal));
                }
                batch.vertexCount = static_cast<int>(vertices_.size()) - batch.firstVertex;
                batches_.push_back(batch);
                pickTriangles_.insert(pickTriangles_.end(), triangles.begin(), triangles.end());
            } catch (const Standard_Failure& error) {
                qWarning() << "Could not tessellate CAD object" << object.name << error.GetMessageString();
            } catch (const std::exception& error) {
                qWarning() << "Could not tessellate CAD object" << object.name << error.what();
            }
        }
    }

    if (!previewShape_.IsNull()) {
        try {
            const std::vector<CadTriangle> previewTriangles = CadIO::triangulate(previewShape_);
            if (!previewTriangles.empty()) {
                Batch batch{-2, static_cast<int>(vertices_.size()), 0, QColor(75, 224, 193, 88)};
                for (const CadTriangle& triangle : previewTriangles) {
                    vertices_.push_back(makeVertex(triangle.a, triangle.normal));
                    vertices_.push_back(makeVertex(triangle.b, triangle.normal));
                    vertices_.push_back(makeVertex(triangle.c, triangle.normal));
                }
                batch.vertexCount = static_cast<int>(vertices_.size()) - batch.firstVertex;
                batches_.push_back(batch);
            }
            appendEdges(previewShape_, -2, QColor(127, 255, 225, 235));
        } catch (const Standard_Failure& error) {
            qWarning() << "Could not tessellate preview shape" << error.GetMessageString();
        } catch (const std::exception& error) {
            qWarning() << "Could not tessellate preview shape" << error.what();
        }
    }

    uploadBuffers();
    update();
}

void CadViewport::uploadBuffers()
{
    if (!glReady_ || !context() || !context()->isValid())
        return;
    const bool restoreContext = QOpenGLContext::currentContext() != context();
    if (restoreContext)
        makeCurrent();

    if (vertexBuffer_.isCreated()) {
        vertexBuffer_.bind();
        const std::size_t bytes = vertices_.size() * sizeof(Vertex);
        vertexBuffer_.allocate(vertices_.empty() ? nullptr : vertices_.data(), static_cast<int>(bytes));
        vertexBuffer_.release();
    }
    if (lineBuffer_.isCreated()) {
        lineBuffer_.bind();
        const std::size_t bytes = lineVertices_.size() * sizeof(Vertex);
        lineBuffer_.allocate(lineVertices_.empty() ? nullptr : lineVertices_.data(), static_cast<int>(bytes));
        lineBuffer_.release();
    }
    if (gridBuffer_.isCreated()) {
        gridBuffer_.bind();
        const std::size_t bytes = gridVertices_.size() * sizeof(Vertex);
        gridBuffer_.allocate(gridVertices_.empty() ? nullptr : gridVertices_.data(), static_cast<int>(bytes));
        gridBuffer_.release();
    }

    if (restoreContext)
        doneCurrent();
}

bool CadViewport::screenRay(const QPoint& point, QVector3D& origin, QVector3D& direction) const
{
    if (width() <= 0 || height() <= 0)
        return false;
    const float x = 2.0F * static_cast<float>(point.x()) / static_cast<float>(width()) - 1.0F;
    const float y = 1.0F - 2.0F * static_cast<float>(point.y()) / static_cast<float>(height());
    bool invertible = false;
    const QMatrix4x4 inverse = (projectionMatrix() * viewMatrix()).inverted(&invertible);
    if (!invertible)
        return false;
    QVector4D nearPoint = inverse * QVector4D(x, y, -1.0F, 1.0F);
    QVector4D farPoint = inverse * QVector4D(x, y, 1.0F, 1.0F);
    if (std::abs(nearPoint.w()) < 1.0e-7F || std::abs(farPoint.w()) < 1.0e-7F)
        return false;
    nearPoint /= nearPoint.w();
    farPoint /= farPoint.w();
    origin = nearPoint.toVector3D();
    direction = (farPoint.toVector3D() - origin).normalized();
    return direction.lengthSquared() > 0.5F;
}

bool CadViewport::planeIntersection(const QPoint& point, float planeZ, QVector3D& result) const
{
    return planeIntersection(point, QVector3D(0.0F, 0.0F, planeZ), QVector3D(0.0F, 0.0F, 1.0F), result);
}

bool CadViewport::planeIntersection(const QPoint& point,
                                   const QVector3D& planePoint,
                                   const QVector3D& planeNormal,
                                   QVector3D& result) const
{
    QVector3D origin, direction;
    if (!screenRay(point, origin, direction))
        return false;
    const float denominator = QVector3D::dotProduct(direction, planeNormal);
    if (std::abs(denominator) < 1.0e-6F)
        return false;
    const float distance = QVector3D::dotProduct(planePoint - origin, planeNormal) / denominator;
    if (distance < 0.0F)
        return false;
    result = origin + direction * distance;
    return true;
}

CadViewport::PickResult CadViewport::pick(const QPoint& point) const
{
    QVector3D origin, direction;
    if (!screenRay(point, origin, direction))
        return {};

    PickResult closest;
    closest.distance = std::numeric_limits<float>::max();
    for (const CadTriangle& triangle : pickTriangles_) {
        float distance = 0.0F;
        if (rayTriangle(origin, direction, triangle, distance) && distance < closest.distance) {
            closest.objectIndex = static_cast<int>(triangle.objectIndex);
            closest.distance = distance;
            closest.point = origin + direction * distance;
        }
    }
    if (closest.objectIndex < 0 && !lineVertices_.empty()) {
        const QMatrix4x4 viewProjection = projectionMatrix() * viewMatrix();
        const auto toScreen = [this, &viewProjection](const Vertex& vertex, QVector2D& screen) {
            const QVector4D clip = viewProjection * QVector4D(vertex.x, vertex.y, vertex.z, 1.0F);
            if (clip.w() <= 1.0e-5F)
                return false;
            screen = QVector2D((clip.x() / clip.w() + 1.0F) * static_cast<float>(width()) * 0.5F,
                               (1.0F - clip.y() / clip.w()) * static_cast<float>(height()) * 0.5F);
            return true;
        };
        float bestPixelDistanceSquared = 36.0F;
        const QVector2D cursor(static_cast<float>(point.x()), static_cast<float>(point.y()));
        for (const LineBatch& batch : lineBatches_) {
            if (batch.objectIndex < 0)
                continue;
            const int end = batch.firstVertex + batch.vertexCount;
            for (int i = batch.firstVertex; i + 1 < end; i += 2) {
                QVector2D a, b;
                if (!toScreen(lineVertices_[static_cast<std::size_t>(i)], a) ||
                    !toScreen(lineVertices_[static_cast<std::size_t>(i + 1)], b))
                    continue;
                const QVector2D segment = b - a;
                const float lengthSquared = segment.lengthSquared();
                const float t = lengthSquared > 1.0e-6F
                    ? std::clamp(QVector2D::dotProduct(cursor - a, segment) / lengthSquared, 0.0F, 1.0F)
                    : 0.0F;
                const float pixelDistanceSquared = (cursor - (a + segment * t)).lengthSquared();
                if (pixelDistanceSquared >= bestPixelDistanceSquared)
                    continue;
                bestPixelDistanceSquared = pixelDistanceSquared;
                const Vertex& va = lineVertices_[static_cast<std::size_t>(i)];
                const Vertex& vb = lineVertices_[static_cast<std::size_t>(i + 1)];
                closest.objectIndex = batch.objectIndex;
                closest.point = QVector3D(va.x, va.y, va.z) * (1.0F - t) + QVector3D(vb.x, vb.y, vb.z) * t;
                closest.distance = (closest.point - cameraPosition()).length();
            }
        }
    }
    if (closest.objectIndex < 0)
        closest.distance = 0.0F;
    return closest;
}

void CadViewport::beginObjectDrag(const QPoint& point)
{
    if (!document_ || selectedObject_ < 0 || static_cast<std::size_t>(selectedObject_) >= document_->objects.size())
        return;
    CadObject& object = document_->objects[static_cast<std::size_t>(selectedObject_)];
    if (object.featureType == QStringLiteral("sketch"))
        return;
    dragStartPosition_ = object.position;
    dragStartRotation_ = object.rotation;
    dragStartScale_ = object.scale;
    dragPlanePoint_ = object.position;
    dragPlaneNormal_ = (target_ - cameraPosition()).normalized();
    if (tool_ == Tool::Move && !planeIntersection(point, dragPlanePoint_, dragPlaneNormal_, dragPlaneStart_))
        return;
    objectDrag_ = true;
    objectChangedDuringDrag_ = false;
}

void CadViewport::updateObjectDrag(const QPoint& point)
{
    if (!objectDrag_ || !document_ || selectedObject_ < 0 ||
        static_cast<std::size_t>(selectedObject_) >= document_->objects.size())
        return;
    const float dx = static_cast<float>(point.x() - pressPosition_.x());
    const float dy = static_cast<float>(point.y() - pressPosition_.y());
    CadObject& object = document_->objects[static_cast<std::size_t>(selectedObject_)];

    if (tool_ == Tool::Move) {
        QVector3D current;
        if (!planeIntersection(point, dragPlanePoint_, dragPlaneNormal_, current))
            return;
        object.position = dragStartPosition_ + (current - dragPlaneStart_);
    } else if (tool_ == Tool::Rotate) {
        object.rotation = dragStartRotation_ + QVector3D(dy * 0.45F, 0.0F, dx * 0.45F);
    } else if (tool_ == Tool::Scale) {
        const float factor = std::clamp(std::exp(-dy * 0.008F), 0.01F, 100.0F);
        object.scale = dragStartScale_ * factor;
    }

    objectChangedDuringDrag_ = true;
    rebuildGeometry();
    emit objectChanged(false);
}

void CadViewport::finishObjectDrag()
{
    if (!objectDrag_)
        return;
    objectDrag_ = false;
    if (objectChangedDuringDrag_)
        emit objectChanged(true);
    objectChangedDuringDrag_ = false;
}

void CadViewport::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    const QPoint point = event->position().toPoint();
    lastPosition_ = point;
    pressPosition_ = point;

    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        navigationButton_ = event->button();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return QOpenGLWidget::mousePressEvent(event);

    for (const ViewCubeFace& face : makeViewCubeFaces(width(), height())) {
        if (face.polygon.containsPoint(QPointF(point), Qt::OddEvenFill)) {
            setStandardView(face.viewName);
            event->accept();
            return;
        }
    }

    if (sketchTool_ != SketchTool::None) {
        leftPressed_ = true;
        QVector3D world;
        const QString plane = planeCode(activeReferencePlane_);
        if (planeIntersection(point, QVector3D(0, 0, 0), cadSketchPlaneNormal(plane), world)) {
            sketchStart_ = snapSketchPoint(cadSketchPointFromWorld(plane, world));
            sketchCurrent_ = sketchStart_;
            sketchDrawing_ = true;
            update();
        }
        event->accept();
        return;
    }

    leftPressed_ = true;
    if (tool_ == Tool::Measure) {
        PickResult result = pick(point);
        QVector3D measurementPoint;
        bool valid = false;
        if (result.objectIndex >= 0) {
            measurementPoint = result.point;
            valid = true;
        } else {
            valid = planeIntersection(point, 0.0F, measurementPoint);
        }
        if (valid) {
            if (!measurementStart_) {
                measurementStart_ = measurementPoint;
            } else {
                emit measurementCompleted((*measurementStart_ - measurementPoint).length());
                measurementStart_.reset();
            }
        }
        event->accept();
        return;
    }

    const PickResult result = pick(point);
    if (result.objectIndex != selectedObject_) {
        selectedObject_ = result.objectIndex;
        emit selectionChanged(selectedObject_);
        update();
    }
    if (selectedObject_ >= 0 && tool_ != Tool::Select)
        beginObjectDrag(point);
    event->accept();
}

void CadViewport::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint point = event->position().toPoint();
    const QPoint delta = point - lastPosition_;

    if (navigationButton_ == Qt::MiddleButton) {
        yawDegrees_ += static_cast<float>(delta.x()) * 0.45F;
        pitchDegrees_ = std::clamp(pitchDegrees_ + static_cast<float>(delta.y()) * 0.35F, -88.0F, 88.0F);
        currentViewName_.clear();
        update();
    } else if (navigationButton_ == Qt::RightButton) {
        const float scale = cameraDistance_ * 2.0F * std::tan(qDegreesToRadians(kFieldOfView * 0.5F)) /
                            static_cast<float>(std::max(1, height()));
        const QVector3D forward = (target_ - cameraPosition()).normalized();
        QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 0, 1));
        if (right.lengthSquared() < 1.0e-5F)
            right = QVector3D::crossProduct(forward, QVector3D(0, 1, 0));
        right.normalize();
        const QVector3D up = QVector3D::crossProduct(right, forward).normalized();
        target_ += right * (-static_cast<float>(delta.x()) * scale) + up * (static_cast<float>(delta.y()) * scale);
        update();
    } else if (sketchTool_ != SketchTool::None && sketchDrawing_) {
        QVector3D world;
        const QString plane = planeCode(activeReferencePlane_);
        if (planeIntersection(point, QVector3D(0, 0, 0), cadSketchPlaneNormal(plane), world)) {
            sketchCurrent_ = snapSketchPoint(cadSketchPointFromWorld(plane, world));
            update();
        }
    } else if (leftPressed_ && objectDrag_ && (point - pressPosition_).manhattanLength() > 1) {
        updateObjectDrag(point);
    }

    QVector3D position;
    if (activeReferencePlane_ != ReferencePlane::None) {
        const QString plane = planeCode(activeReferencePlane_);
        if (planeIntersection(point, QVector3D(0, 0, 0), cadSketchPlaneNormal(plane), position))
            emit worldPositionChanged(position);
    } else if (planeIntersection(point, 0.0F, position)) {
        emit worldPositionChanged(position);
    }
    lastPosition_ = point;
    event->accept();
}

void CadViewport::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        navigationButton_ = Qt::NoButton;
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (sketchTool_ != SketchTool::None && sketchDrawing_) {
            QVector3D world;
            const QString plane = planeCode(activeReferencePlane_);
            if (planeIntersection(event->position().toPoint(), QVector3D(0, 0, 0), cadSketchPlaneNormal(plane), world))
                sketchCurrent_ = snapSketchPoint(cadSketchPointFromWorld(plane, world));

            QString profileType;
            QVector2D origin;
            QVector2D size;
            if (sketchTool_ == SketchTool::Rectangle) {
                const float left = std::min(sketchStart_.x(), sketchCurrent_.x());
                const float right = std::max(sketchStart_.x(), sketchCurrent_.x());
                const float bottom = std::min(sketchStart_.y(), sketchCurrent_.y());
                const float top = std::max(sketchStart_.y(), sketchCurrent_.y());
                origin = QVector2D(left, bottom);
                size = QVector2D(right - left, top - bottom);
                if (size.x() > 0.1F && size.y() > 0.1F)
                    profileType = QStringLiteral("sketch-rectangle");
            } else if (sketchTool_ == SketchTool::Circle) {
                const float radius = (sketchCurrent_ - sketchStart_).length();
                origin = sketchStart_;
                size = QVector2D(radius, 0.0F);
                if (radius > 0.1F)
                    profileType = QStringLiteral("sketch-circle");
            }

            sketchDrawing_ = false;
            if (!profileType.isEmpty()) {
                sketchTool_ = SketchTool::None;
                setCursor(Qt::ArrowCursor);
                emit sketchFinished(profileType, origin, size);
            } else {
                emit sketchFinished(QString(), QVector2D(), QVector2D());
            }
            update();
        } else {
            finishObjectDrag();
        }
        leftPressed_ = false;
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void CadViewport::wheelEvent(QWheelEvent* event)
{
    const float steps = static_cast<float>(event->angleDelta().y()) / 120.0F;
    cameraDistance_ = std::clamp(cameraDistance_ * std::exp(-steps * 0.12F), 1.0F, 1000000.0F);
    update();
    event->accept();
}

void CadViewport::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && sketchTool_ != SketchTool::None) {
        cancelSketch();
        emit sketchFinished(QString(), QVector2D(), QVector2D());
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void CadViewport::fitAll()
{
    if (pickTriangles_.empty() && lineVertices_.empty()) {
        target_ = QVector3D(0, 0, 0);
        cameraDistance_ = 320.0F;
        update();
        return;
    }

    QVector3D minimum(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    QVector3D maximum(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    const auto include = [&minimum, &maximum](const QVector3D& point) {
        minimum.setX(std::min(minimum.x(), point.x()));
        minimum.setY(std::min(minimum.y(), point.y()));
        minimum.setZ(std::min(minimum.z(), point.z()));
        maximum.setX(std::max(maximum.x(), point.x()));
        maximum.setY(std::max(maximum.y(), point.y()));
        maximum.setZ(std::max(maximum.z(), point.z()));
    };
    for (const CadTriangle& triangle : pickTriangles_) {
        include(triangle.a);
        include(triangle.b);
        include(triangle.c);
    }
    for (const Vertex& vertex : lineVertices_)
        include(QVector3D(vertex.x, vertex.y, vertex.z));

    target_ = (minimum + maximum) * 0.5F;
    const QVector3D size = maximum - minimum;
    const float largest = std::max({size.x(), size.y(), size.z(), 10.0F});
    cameraDistance_ = largest / (2.0F * std::tan(qDegreesToRadians(kFieldOfView * 0.5F))) * 1.35F;
    update();
}
