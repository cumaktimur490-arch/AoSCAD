#include "CadViewport.hpp"

#include <Standard_Failure.hxx>

#include <QDebug>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLShader>
#include <QVector4D>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
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
        gridBuffer_.destroy();
        doneCurrent();
    }
}

void CadViewport::setDocument(CadDocument* document)
{
    document_ = document;
    selectedObject_ = -1;
    rebuildGeometry();
    update();
}

void CadViewport::setSelectedObject(int index)
{
    if (!document_ || index < 0 || static_cast<std::size_t>(index) >= document_->objects.size())
        index = -1;
    if (selectedObject_ == index)
        return;
    selectedObject_ = index;
    update();
}

void CadViewport::setTool(Tool tool)
{
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
        yawDegrees_ = -90.0F;
        pitchDegrees_ = 89.0F;
    } else if (viewName == QStringLiteral("front")) {
        yawDegrees_ = -90.0F;
        pitchDegrees_ = 0.0F;
    } else if (viewName == QStringLiteral("right")) {
        yawDegrees_ = 0.0F;
        pitchDegrees_ = 0.0F;
    } else {
        yawDegrees_ = -45.0F;
        pitchDegrees_ = 28.0F;
    }
    update();
}

void CadViewport::initializeGL()
{
    initializeOpenGLFunctions();
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);
    glClearColor(0.067F, 0.090F, 0.114F, 1.0F);

    const char* vertexShader = R"(
        #version 330 core
        in vec3 aPosition;
        in vec3 aNormal;
        uniform mat4 uViewProjection;
        out vec3 vNormal;
        void main() {
            gl_Position = uViewProjection * vec4(aPosition, 1.0);
            vNormal = aNormal;
        }
    )";
    const char* fragmentShader = R"(
        #version 330 core
        in vec3 vNormal;
        uniform vec4 uColor;
        uniform bool uLit;
        out vec4 fragColor;
        void main() {
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
    gridBuffer_.create();
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

    QMatrix4x4 viewProjection = projectionMatrix() * viewMatrix();
    vertexArray_.bind();
    shader_.bind();
    shader_.setUniformValue("uViewProjection", viewProjection);

    if (gridVisible_ && gridBuffer_.isCreated() && !gridVertices_.empty()) {
        gridBuffer_.bind();
        shader_.setUniformValue("uColor", colorVector(QColor(44, 59, 68)));
        shader_.setUniformValue("uLit", false);
        shader_.enableAttributeArray(0);
        shader_.enableAttributeArray(1);
        shader_.setAttributeBuffer(0, GL_FLOAT, static_cast<int>(offsetof(Vertex, x)), 3, sizeof(Vertex));
        shader_.setAttributeBuffer(1, GL_FLOAT, static_cast<int>(offsetof(Vertex, nx)), 3, sizeof(Vertex));
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
            QColor color = batch.color;
            if (batch.objectIndex == selectedObject_)
                color = QColor(103, 218, 192);
            shader_.setUniformValue("uColor", colorVector(color));
            glDrawArrays(GL_TRIANGLES, batch.firstVertex, batch.vertexCount);
        }
        vertexBuffer_.release();
    }
    shader_.disableAttributeArray(0);
    shader_.disableAttributeArray(1);
    shader_.release();
    vertexArray_.release();
}

void CadViewport::rebuildGrid()
{
    gridVertices_.clear();
    constexpr int halfLines = 50;
    constexpr float spacing = 10.0F;
    for (int i = -halfLines; i <= halfLines; ++i) {
        const float at = static_cast<float>(i) * spacing;
        gridVertices_.push_back(makeVertex(QVector3D(at, -halfLines * spacing, -0.1F), QVector3D(0, 0, 1)));
        gridVertices_.push_back(makeVertex(QVector3D(at, halfLines * spacing, -0.1F), QVector3D(0, 0, 1)));
        gridVertices_.push_back(makeVertex(QVector3D(-halfLines * spacing, at, -0.1F), QVector3D(0, 0, 1)));
        gridVertices_.push_back(makeVertex(QVector3D(halfLines * spacing, at, -0.1F), QVector3D(0, 0, 1)));
    }
    uploadBuffers();
}

void CadViewport::rebuildGeometry()
{
    vertices_.clear();
    batches_.clear();
    pickTriangles_.clear();
    if (!document_) {
        uploadBuffers();
        update();
        return;
    }

    for (std::size_t index = 0; index < document_->objects.size(); ++index) {
        const CadObject& object = document_->objects[index];
        if (!object.visible || object.shape.IsNull())
            continue;
        try {
            const std::vector<CadTriangle> triangles = CadIO::triangulate(object.worldShape(), index);
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
    if (closest.objectIndex < 0)
        closest.distance = 0.0F;
    return closest;
}

void CadViewport::beginObjectDrag(const QPoint& point)
{
    if (!document_ || selectedObject_ < 0 || static_cast<std::size_t>(selectedObject_) >= document_->objects.size())
        return;
    CadObject& object = document_->objects[static_cast<std::size_t>(selectedObject_)];
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
    } else if (leftPressed_ && objectDrag_ && (point - pressPosition_).manhattanLength() > 1) {
        updateObjectDrag(point);
    }

    QVector3D position;
    if (planeIntersection(point, 0.0F, position))
        emit worldPositionChanged(position);
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
        finishObjectDrag();
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

void CadViewport::fitAll()
{
    if (pickTriangles_.empty()) {
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

    target_ = (minimum + maximum) * 0.5F;
    const QVector3D size = maximum - minimum;
    const float largest = std::max({size.x(), size.y(), size.z(), 10.0F});
    cameraDistance_ = largest / (2.0F * std::tan(qDegreesToRadians(kFieldOfView * 0.5F))) * 1.35F;
    update();
}
