#include "MainWindow.hpp"
#include "ExpressionParser.hpp"

#include <Standard_Failure.hxx>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHeaderView>
#include <QAbstractSpinBox>
#include <QAbstractItemView>
#include <QSet>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QFrame>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <utility>

namespace {
constexpr int kTreeNodeTypeRole = Qt::UserRole + 1;
constexpr int kReferencePlaneRole = Qt::UserRole + 2;

enum class TreeNodeType { Origin, ReferencePlane, Object };

bool sameDocumentState(const CadDocument& left, const CadDocument& right)
{
    if (left.name != right.name || left.units != right.units || left.variables != right.variables ||
        left.objects.size() != right.objects.size())
        return false;
    for (std::size_t i = 0; i < left.objects.size(); ++i) {
        const CadObject& a = left.objects[i];
        const CadObject& b = right.objects[i];
        if (a.id != b.id || a.name != b.name || a.featureType != b.featureType || a.inputs != b.inputs ||
            a.suppressed != b.suppressed || a.color != b.color || a.position != b.position ||
            a.rotation != b.rotation || a.scale != b.scale || a.visible != b.visible ||
            a.primitiveType != b.primitiveType || a.primitiveParameters != b.primitiveParameters ||
            a.parameterExpressions != b.parameterExpressions || a.sketchPlane != b.sketchPlane ||
            a.sketchOrigin != b.sketchOrigin || a.sketchConstraints != b.sketchConstraints ||
            a.symmetricExtrude != b.symmetricExtrude ||
            a.materialId != b.materialId || a.shape.IsNull() != b.shape.IsNull())
            return false;
        if (a.featureType == QStringLiteral("imported-body") && !a.shape.IsNull() && !a.shape.IsSame(b.shape))
            return false;
    }
    return true;
}

QString openFilter()
{
    return QStringLiteral("CAD-файлы (*.step *.stp *.iges *.igs *.brep *.brp *.stl *.obj *.sldprt *.sldasm *.slddrw *.tfc *.grb);;Все файлы (*.*)");
}

QString saveFilter(const QString& extension)
{
    if (extension == QStringLiteral("stl")) return QStringLiteral("STL-сетка (*.stl)");
    if (extension == QStringLiteral("step")) return QStringLiteral("STEP CAD (*.step *.stp)");
    if (extension == QStringLiteral("iges")) return QStringLiteral("IGES CAD (*.iges *.igs)");
    if (extension == QStringLiteral("obj")) return QStringLiteral("OBJ-сетка (*.obj)");
    if (extension == QStringLiteral("brep")) return QStringLiteral("Open CASCADE BREP (*.brep)");
    return QStringLiteral("Проект AoSCAD (*.aoscad)");
}

QDoubleSpinBox* makeNumberField(QWidget* parent, double minimum, double maximum, int decimals, double step)
{
    auto* field = new QDoubleSpinBox(parent);
    field->setRange(minimum, maximum);
    field->setDecimals(decimals);
    field->setSingleStep(step);
    field->setKeyboardTracking(false);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setAlignment(Qt::AlignRight);
    return field;
}

QWidget* xyzRow(const QStringList& labels,
                std::array<QDoubleSpinBox*, 3>& fields,
                QWidget* parent,
                double minimum,
                double maximum,
                int decimals,
                double step)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    for (int i = 0; i < 3; ++i) {
        auto* cell = new QWidget(row);
        auto* cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 0, 0);
        cellLayout->setSpacing(3);
        auto* label = new QLabel(labels[i], cell);
        label->setObjectName(QStringLiteral("axisLabel"));
        label->setAlignment(Qt::AlignCenter);
        fields[static_cast<std::size_t>(i)] = makeNumberField(cell, minimum, maximum, decimals, step);
        cellLayout->addWidget(label);
        cellLayout->addWidget(fields[static_cast<std::size_t>(i)]);
        layout->addWidget(cell);
    }
    return row;
}

QIcon makeRibbonIcon(const QString& name, bool darkTheme)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor iconLine = darkTheme ? QColor(QStringLiteral("#CBD3D7")) : QColor(QStringLiteral("#405B70"));
    painter.setPen(QPen(iconLine, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(darkTheme ? QColor(QStringLiteral("#71858E")) : QColor(QStringLiteral("#D9EAF6")));

    const auto polygon = [&painter](std::initializer_list<QPointF> points, const QColor& fill) {
        QPolygonF shape;
        for (const QPointF& point : points)
            shape << point;
        painter.setBrush(fill);
        painter.drawPolygon(shape);
    };
    const auto drawCube = [&polygon] {
        polygon({QPointF(12, 2.5), QPointF(21, 7), QPointF(12, 11.5), QPointF(3, 7)}, QColor(QStringLiteral("#E8F3FA")));
        polygon({QPointF(3, 7), QPointF(12, 11.5), QPointF(12, 21.5), QPointF(3, 17)}, QColor(QStringLiteral("#C8E0F1")));
        polygon({QPointF(12, 11.5), QPointF(21, 7), QPointF(21, 17), QPointF(12, 21.5)}, QColor(QStringLiteral("#B4D2E6")));
    };

    if (name == QStringLiteral("box") || name == QStringLiteral("iso")) {
        drawCube();
    } else if (name == QStringLiteral("new")) {
        QPainterPath page;
        page.moveTo(6, 2.5); page.lineTo(15, 2.5); page.lineTo(19, 6.5); page.lineTo(19, 21.5);
        page.lineTo(6, 21.5); page.closeSubpath();
        painter.drawPath(page);
        painter.drawLine(14.5, 2.8, 14.5, 7); painter.drawLine(14.5, 7, 18.7, 7);
        painter.drawLine(12.5, 10, 12.5, 18); painter.drawLine(8.5, 14, 16.5, 14);
    } else if (name == QStringLiteral("open")) {
        QPainterPath folder;
        folder.moveTo(2.5, 7); folder.lineTo(9, 7); folder.lineTo(11, 9); folder.lineTo(21.5, 9);
        folder.lineTo(19, 19.5); folder.lineTo(3, 19.5); folder.closeSubpath();
        painter.drawPath(folder);
        painter.drawLine(2.5, 7, 3.5, 4.5); painter.drawLine(3.5, 4.5, 9, 4.5); painter.drawLine(9, 4.5, 10.5, 7);
    } else if (name == QStringLiteral("import")) {
        QPainterPath folder;
        folder.moveTo(2.5, 7); folder.lineTo(8.5, 7); folder.lineTo(10.5, 9); folder.lineTo(21.5, 9);
        folder.lineTo(19, 20); folder.lineTo(3, 20); folder.closeSubpath();
        painter.drawPath(folder);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(12, 2.5, 12, 13); painter.drawLine(8.5, 9.5, 12, 13); painter.drawLine(15.5, 9.5, 12, 13);
    } else if (name == QStringLiteral("save")) {
        painter.drawRoundedRect(QRectF(3.5, 2.5, 17, 19), 1.5, 1.5);
        painter.drawRect(QRectF(7, 3.5, 10, 6));
        painter.drawRect(QRectF(7, 13, 10, 7));
        painter.drawLine(9, 4.5, 9, 7.5); painter.drawLine(15, 4.5, 15, 7.5);
    } else if (name == QStringLiteral("undo") || name == QStringLiteral("redo")) {
        const bool undo = name == QStringLiteral("undo");
        QPainterPath arrow;
        arrow.moveTo(12, 5); arrow.cubicTo(19, 5, 20, 9, 20, 13);
        arrow.cubicTo(20, 18, 16, 19, 11, 19);
        painter.drawPath(arrow);
        painter.drawLine(12, 5, undo ? 6 : 18, 5);
        painter.drawLine(undo ? 6 : 18, 5, undo ? 9 : 15, 2.5);
        painter.drawLine(undo ? 6 : 18, 5, undo ? 9 : 15, 8);
    } else if (name == QStringLiteral("duplicate")) {
        painter.drawRoundedRect(QRectF(7, 3, 13, 15), 1, 1);
        painter.drawRoundedRect(QRectF(3, 7, 13, 14), 1, 1);
    } else if (name == QStringLiteral("delete")) {
        painter.drawLine(4, 6, 20, 6); painter.drawLine(9, 3.5, 15, 3.5);
        painter.drawRoundedRect(QRectF(6, 7, 12, 14), 1, 1);
        painter.drawLine(10, 10, 10, 18); painter.drawLine(14, 10, 14, 18);
    } else if (name == QStringLiteral("select")) {
        QPainterPath cursor;
        cursor.moveTo(5, 2.5); cursor.lineTo(18.5, 14); cursor.lineTo(12.7, 14.5);
        cursor.lineTo(15.5, 20.5); cursor.lineTo(12.5, 21.5); cursor.lineTo(9.5, 15.5);
        cursor.lineTo(5, 20); cursor.closeSubpath();
        painter.drawPath(cursor);
    } else if (name == QStringLiteral("move")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(12, 4, 12, 20); painter.drawLine(4, 12, 20, 12);
        painter.drawLine(12, 4, 9.5, 7); painter.drawLine(12, 4, 14.5, 7);
        painter.drawLine(12, 20, 9.5, 17); painter.drawLine(12, 20, 14.5, 17);
        painter.drawLine(4, 12, 7, 9.5); painter.drawLine(4, 12, 7, 14.5);
        painter.drawLine(20, 12, 17, 9.5); painter.drawLine(20, 12, 17, 14.5);
    } else if (name == QStringLiteral("rotate")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(4, 4, 16, 16), 35 * 16, 285 * 16);
        painter.drawLine(18, 4, 20, 8); painter.drawLine(18, 4, 14, 5);
    } else if (name == QStringLiteral("scale")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(5, 5, 14, 14));
        painter.drawLine(7, 17, 17, 7); painter.drawLine(17, 7, 13.5, 7); painter.drawLine(17, 7, 17, 10.5);
    } else if (name == QStringLiteral("measure")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(4, 17, 20, 7); painter.drawLine(5, 13, 8, 18); painter.drawLine(9, 11, 12, 16);
        painter.drawLine(13, 9, 16, 14); painter.drawLine(17, 6, 20, 11);
    } else if (name == QStringLiteral("cylinder")) {
        painter.drawEllipse(QRectF(4, 3, 16, 6));
        painter.drawLine(4, 6, 4, 18); painter.drawLine(20, 6, 20, 18);
        const QPen cylinderPen = painter.pen();
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(cylinderPen.color(), cylinderPen.widthF(), Qt::DashLine));
        painter.drawArc(QRectF(4, 15, 16, 6), 180 * 16, 180 * 16);
        painter.setPen(cylinderPen);
        painter.drawArc(QRectF(4, 15, 16, 6), 0, 180 * 16);
    } else if (name == QStringLiteral("sphere")) {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        painter.setBrush(Qt::NoBrush); painter.drawArc(QRectF(6, 4, 9, 16), 70 * 16, 215 * 16);
    } else if (name == QStringLiteral("cone")) {
        painter.drawEllipse(QRectF(4, 16, 16, 5));
        painter.drawLine(12, 3, 4, 18); painter.drawLine(12, 3, 20, 18);
        painter.setBrush(Qt::NoBrush); painter.drawArc(QRectF(4, 16, 16, 5), 0, 180 * 16);
    } else if (name == QStringLiteral("torus")) {
        painter.drawEllipse(QRectF(3, 5, 18, 14));
        painter.setBrush(darkTheme ? QColor(QStringLiteral("#2B2B2B")) : QColor(QStringLiteral("#F0F0F0")));
        painter.drawEllipse(QRectF(8, 9, 8, 6));
    } else if (name == QStringLiteral("rectangle")) {
        painter.drawRect(QRectF(4, 5, 16, 14));
        painter.setBrush(Qt::NoBrush); painter.drawLine(4, 12, 20, 12); painter.drawLine(12, 5, 12, 19);
    } else if (name == QStringLiteral("circle")) {
        painter.drawEllipse(QRectF(4, 4, 16, 16));
        painter.setBrush(Qt::NoBrush); painter.drawLine(12, 4, 12, 20); painter.drawLine(4, 12, 20, 12);
    } else if (name == QStringLiteral("extrude")) {
        polygon({QPointF(4, 13), QPointF(12, 9), QPointF(20, 13), QPointF(12, 17)}, QColor(QStringLiteral("#D9EAF6")));
        painter.drawLine(12, 14, 12, 4); painter.drawLine(12, 4, 9.5, 7); painter.drawLine(12, 4, 14.5, 7);
        painter.drawLine(4, 13, 4, 18); painter.drawLine(20, 13, 20, 18); painter.drawLine(4, 18, 12, 22); painter.drawLine(20, 18, 12, 22);
    } else if (name == QStringLiteral("fillet")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(4, 4, 16, 16), 4, 4);
        painter.drawArc(QRectF(7, 7, 10, 10), 0, 90 * 16);
    } else if (name == QStringLiteral("chamfer")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(4, 4, 16, 16));
        painter.drawLine(15, 4, 20, 9); painter.drawLine(15, 4, 15, 9); painter.drawLine(15, 9, 20, 9);
    } else if (name == QStringLiteral("union") || name == QStringLiteral("cut") || name == QStringLiteral("common")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(3, 5, 12, 14), 2, 2);
        painter.drawRoundedRect(QRectF(9, 5, 12, 14), 2, 2);
        if (name == QStringLiteral("union")) {
            painter.drawLine(8, 5, 8, 19); painter.drawLine(16, 5, 16, 19);
        } else if (name == QStringLiteral("cut")) {
            painter.drawLine(9, 5, 9, 19);
        } else {
            painter.drawLine(11, 5, 11, 19); painter.drawLine(13, 5, 13, 19);
        }
    } else if (name == QStringLiteral("pattern")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(2.5, 8, 6, 8));
        painter.drawRect(QRectF(9, 8, 6, 8));
        painter.drawRect(QRectF(15.5, 8, 6, 8));
        painter.drawLine(4, 5, 20, 5); painter.drawLine(17, 3, 20, 5); painter.drawLine(17, 7, 20, 5);
    } else if (name == QStringLiteral("mirror")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(2, 7, 7, 10)); painter.drawRect(QRectF(15, 7, 7, 10));
        const QPen mirrorPen = painter.pen();
        painter.setPen(QPen(mirrorPen.color(), mirrorPen.widthF(), Qt::DashLine));
        painter.drawLine(12, 3, 12, 21);
    } else if (name == QStringLiteral("fit")) {
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(4, 9, 4, 4); painter.drawLine(4, 4, 9, 4);
        painter.drawLine(15, 4, 20, 4); painter.drawLine(20, 4, 20, 9);
        painter.drawLine(4, 15, 4, 20); painter.drawLine(4, 20, 9, 20);
        painter.drawLine(15, 20, 20, 20); painter.drawLine(20, 20, 20, 15);
        painter.drawLine(8, 8, 11, 11); painter.drawLine(16, 8, 13, 11);
        painter.drawLine(8, 16, 11, 13); painter.drawLine(16, 16, 13, 13);
    } else if (name == QStringLiteral("grid")) {
        painter.drawRect(QRectF(3, 3, 18, 18));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(9, 3, 9, 21); painter.drawLine(15, 3, 15, 21);
        painter.drawLine(3, 9, 21, 9); painter.drawLine(3, 15, 21, 15);
    } else if (name == QStringLiteral("top")) {
        painter.drawRect(QRectF(4, 5, 16, 14));
        painter.setBrush(Qt::NoBrush); painter.drawLine(7, 12, 17, 12); painter.drawLine(12, 8, 12, 16);
    } else if (name == QStringLiteral("front")) {
        painter.drawRect(QRectF(4, 4, 16, 16));
        painter.setBrush(Qt::NoBrush); painter.drawLine(4, 12, 20, 12);
    } else if (name == QStringLiteral("right")) {
        painter.drawRect(QRectF(4, 4, 16, 16));
        painter.setBrush(Qt::NoBrush); painter.drawLine(12, 4, 12, 20);
    } else {
        drawCube();
    }

    painter.end();
    return QIcon(pixmap);
}
} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    QSettings settings;
    darkTheme_ = settings.value(QStringLiteral("appearance/theme"), QStringLiteral("dark")).toString() !=
                 QStringLiteral("light");

    resize(1440, 900);
    setMinimumSize(960, 640);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
    createInterface();
    createMenusAndRibbon();
    connectActions();
    applyTheme(darkTheme_);
    updateTree();
    updateInspector();
    updateStatus();
    pushHistory(true);
    updateWindowTitle();
}

void MainWindow::applyTheme(bool dark)
{
    darkTheme_ = dark;

    const QString background = dark ? QStringLiteral("#2B2B2B") : QStringLiteral("#F0F0F0");
    const QString text = dark ? QStringLiteral("#D2D2D2") : QStringLiteral("#252525");
    const QString chrome = dark ? QStringLiteral("#1D1D1D") : QStringLiteral("#E6E6E6");
    const QString edge = dark ? QStringLiteral("#171717") : QStringLiteral("#C8C8C8");
    const QString accent = dark ? QStringLiteral("#22C9D0") : QStringLiteral("#0078D7");
    const QString accentSurface = dark ? QStringLiteral("#24484B") : QStringLiteral("#CCE4F7");
    const QString menuSurface = dark ? QStringLiteral("#252525") : QStringLiteral("#E6E6E6");
    const QString border = dark ? QStringLiteral("#484848") : QStringLiteral("#C8C8C8");
    const QString hover = dark ? QStringLiteral("#393939") : QStringLiteral("#FFFFFF");
    const QString tabSurface = dark ? QStringLiteral("#252525") : QStringLiteral("#E6E6E6");
    const QString tabPane = dark ? QStringLiteral("#303030") : QStringLiteral("#F0F0F0");
    const QString tabActive = dark ? QStringLiteral("#373737") : QStringLiteral("#F0F0F0");
    const QString inputSurface = dark ? QStringLiteral("#202020") : QStringLiteral("#FFFFFF");
    const QString inputBorder = dark ? QStringLiteral("#474747") : QStringLiteral("#C8C8C8");
    const QString selection = dark ? QStringLiteral("#365E62") : QStringLiteral("#CCE4F7");
    const QString selectionText = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#202020");
    const QString alternate = dark ? QStringLiteral("#2A2A2A") : QStringLiteral("#FFFFFF");
    const QString button = dark ? QStringLiteral("#363636") : QStringLiteral("#E6E6E6");
    const QString buttonHover = dark ? QStringLiteral("#454545") : QStringLiteral("#FFFFFF");
    const QString buttonBorder = dark ? QStringLiteral("#505050") : QStringLiteral("#C8C8C8");
    const QString primaryButton = dark ? QStringLiteral("#245257") : QStringLiteral("#CCE4F7");
    const QString primaryHover = dark ? QStringLiteral("#2B666B") : QStringLiteral("#D9ECFA");
    const QString muted = dark ? QStringLiteral("#AEB7BB") : QStringLiteral("#5B6570");
    const QString statusSurface = dark ? QStringLiteral("#1D1D1D") : QStringLiteral("#F0F0F0");
    const QString statusText = dark ? QStringLiteral("#B8B8B8") : QStringLiteral("#5F6770");
    const QString hintSurface = dark ? QStringLiteral("#222222") : QStringLiteral("#FFFFFF");
    const QString hintText = dark ? QStringLiteral("#B0B8BC") : QStringLiteral("#5F6B75");
    const QString error = dark ? QStringLiteral("#F08A83") : QStringLiteral("#B42318");
    const QString success = dark ? QStringLiteral("#65D3A3") : QStringLiteral("#1E6B50");
    const QString groupBorder = dark ? QStringLiteral("#414141") : QStringLiteral("#C8C8C8");
    const QString separator = dark ? QStringLiteral("#4B4B4B") : QStringLiteral("#C8C8C8");

    const QString styleSheet = QStringLiteral(R"(
        QMainWindow, QWidget { background:%1; color:%2; font-family:"Segoe UI", sans-serif; font-size:9pt; }
        QMenuBar, QToolBar { background:%3; color:%2; border:1px solid %4; }
        QMenuBar::item { padding:4px 8px; }
        QMenuBar::item:selected, QMenu::item:selected { background:%6; color:%16; border:1px solid %5; }
        QMenu { background:%7; color:%2; border:1px solid %8; }
        QMenu::item { padding:6px 24px 6px 10px; }
        QToolBar { spacing:0; padding:0; border-bottom:1px solid %4; }
        QToolBar QToolButton { background:transparent; color:%2; border:1px solid transparent; padding:3px; }
        QToolBar QToolButton:hover { background:%9; border-color:%8; }
        QToolBar QToolButton:checked { background:%6; color:%16; border-color:%5; }
        QTabWidget::pane { background:%11; border:1px solid %4; }
        QTabBar::tab { background:%10; color:%2; border:1px solid %8; padding:5px 14px; margin-right:1px; }
        QTabBar::tab:selected { background:%12; color:%2; border-bottom-color:%12; }
        QToolButton#ribbonButton { background:%1; color:%2; border:1px solid transparent; padding:2px; }
        QToolButton#ribbonButton:hover { background:%9; border-color:%8; }
        QToolButton#ribbonButton:checked { background:%6; color:%16; border:1px solid %5; }
        QDockWidget { border:1px solid %4; }
        QDockWidget::title { background:%7; color:%2; padding:6px 8px; border-bottom:1px solid %8; font-weight:600; }
        QLineEdit, QComboBox, QDoubleSpinBox, QTreeWidget, QTableWidget { background:%13; color:%2; border:1px solid %14; padding:4px 6px; selection-background-color:%15; selection-color:%16; }
        QTreeWidget, QTableWidget { background:%7; alternate-background-color:%17; padding:2px; }
        QTreeWidget::item, QTableWidget::item { padding:4px; }
        QTreeWidget::item:selected, QTableWidget::item:selected, QAbstractItemView::item:selected { background:%15; color:%16; border:1px solid %5; }
        QPushButton { background:%18; color:%2; border:1px solid %20; padding:6px 9px; }
        QPushButton:hover { background:%19; border-color:%5; }
        QPushButton#accentButton { background:%21; color:%16; border-color:%5; font-weight:700; }
        QPushButton#accentButton:hover { background:%22; }
        QGroupBox { border:1px solid %30; margin-top:10px; padding:8px; font-weight:600; }
        QGroupBox::title { subcontrol-origin:margin; left:8px; padding:0 5px; color:%23; }
        QLabel#axisLabel, QLabel#ribbonGroupCaption { color:%23; font-size:8pt; }
        QLabel#sectionTitle { color:%23; font-size:8pt; font-weight:700; letter-spacing:1px; padding:3px 2px; }
        QLabel#objectSectionTitle { color:%23; font-size:8pt; font-weight:700; letter-spacing:1px; margin-top:8px; }
        QLabel#errorLabel { color:%28; font-size:8pt; }
        QLabel#hintLabel { color:%27; font-size:8pt; padding:8px; background:%26; border:1px solid %14; }
        QLabel#extrudeStatusLabel[statusLevel="success"] { color:%29; font-size:8pt; }
        QLabel#extrudeStatusLabel[statusLevel="error"] { color:%28; font-size:8pt; }
        QStatusBar { background:%24; color:%25; border-top:1px solid %4; }
        QFrame#ribbonSeparator { color:%31; }
    )")
        .arg(background).arg(text).arg(chrome).arg(edge).arg(accent).arg(accentSurface).arg(menuSurface)
        .arg(border).arg(hover).arg(tabSurface).arg(tabPane).arg(tabActive).arg(inputSurface).arg(inputBorder)
        .arg(selection).arg(selectionText).arg(alternate).arg(button).arg(buttonHover).arg(buttonBorder)
        .arg(primaryButton).arg(primaryHover).arg(muted).arg(statusSurface).arg(statusText).arg(hintSurface)
        .arg(hintText).arg(error).arg(success).arg(groupBorder).arg(separator);
    setStyleSheet(styleSheet);

    if (viewport_)
        viewport_->setDarkTheme(darkTheme_);
    for (QAction* action : findChildren<QAction*>()) {
        const QString iconName = action->property("ribbonIconName").toString();
        if (!iconName.isEmpty())
            action->setIcon(makeRibbonIcon(iconName, darkTheme_));
    }
    if (darkThemeAction_)
        darkThemeAction_->setChecked(darkTheme_);
    if (lightThemeAction_)
        lightThemeAction_->setChecked(!darkTheme_);
    if (colorButton_)
        updateInspector(false);

    QSettings settings;
    settings.setValue(QStringLiteral("appearance/theme"), darkTheme_ ? QStringLiteral("dark") : QStringLiteral("light"));
}

void MainWindow::setExtrudeStatusLevel(const QString& level)
{
    if (!extrudeStatusLabel_)
        return;
    extrudeStatusLabel_->setProperty("statusLevel", level);
    extrudeStatusLabel_->style()->unpolish(extrudeStatusLabel_);
    extrudeStatusLabel_->style()->polish(extrudeStatusLabel_);
}

void MainWindow::createInterface()
{
    viewport_ = new CadViewport(&document_, this);
    viewport_->setObjectName(QStringLiteral("cadViewport"));
    setCentralWidget(viewport_);
    createProjectDock();
    createInspectorDock();

    coordinateLabel_ = new QLabel(QStringLiteral("X 0.00   Y 0.00   Z 0.00 мм"), this);
    objectCountLabel_ = new QLabel(QStringLiteral("0 объектов"), this);
    statusBar()->addPermanentWidget(coordinateLabel_, 1);
    statusBar()->addPermanentWidget(objectCountLabel_);
}

void MainWindow::createProjectDock()
{
    auto* dock = new QDockWidget(QStringLiteral("Дерево модели"), this);
    projectDock_ = dock;
    dock->setObjectName(QStringLiteral("projectDock"));
    dock->setMinimumWidth(240);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* panel = new QWidget(dock);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(7, 7, 7, 7);
    layout->setSpacing(6);

    projectName_ = new QLineEdit(document_.name, panel);
    projectName_->setPlaceholderText(QStringLiteral("Название проекта"));
    projectName_->setClearButtonEnabled(true);
    layout->addWidget(projectName_);

    auto* title = new QLabel(QStringLiteral("КОМПОНЕНТЫ"), panel);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);

    tree_ = new QTreeWidget(panel);
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setIndentation(14);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(tree_, 1);

    dock->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    connect(projectName_, &QLineEdit::editingFinished, this, [this] {
        const QString value = projectName_->text().trimmed();
        if (!value.isEmpty()) {
            document_.name = value;
            pushHistory();
        }
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current) {
        const auto nodeType = current
            ? static_cast<TreeNodeType>(current->data(0, kTreeNodeTypeRole).toInt())
            : TreeNodeType::Origin;
        if (nodeType == TreeNodeType::ReferencePlane) {
            const auto plane = static_cast<CadViewport::ReferencePlane>(
                current->data(0, kReferencePlaneRole).toInt());
            viewport_->setSelectedObject(-1);
            viewport_->setActiveReferencePlane(plane);
            if (plane == CadViewport::ReferencePlane::XY)
                viewport_->setStandardView(QStringLiteral("top"));
            else if (plane == CadViewport::ReferencePlane::YZ)
                viewport_->setStandardView(QStringLiteral("right"));
            else if (plane == CadViewport::ReferencePlane::ZX)
                viewport_->setStandardView(QStringLiteral("front"));
            statusBar()->showMessage(QStringLiteral("Выбрана опорная плоскость %1").arg(current->text(0)), 4000);
        } else {
            viewport_->setActiveReferencePlane(CadViewport::ReferencePlane::None);
            const int index = nodeType == TreeNodeType::Object && current
                ? current->data(0, Qt::UserRole).toInt()
                : -1;
            viewport_->setSelectedObject(index);
        }
        updateInspector();
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0 || !item ||
            item->data(0, kTreeNodeTypeRole).toInt() != static_cast<int>(TreeNodeType::Object))
            return;
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size())
            return;
        document_.objects[static_cast<std::size_t>(index)].visible = item->checkState(0) == Qt::Checked;
        refreshViewport();
        pushHistory();
    });
}

void MainWindow::createInspectorDock()
{
    auto* dock = new QDockWidget(QStringLiteral("Свойства"), this);
    inspectorDock_ = dock;
    dock->setObjectName(QStringLiteral("inspectorDock"));
    dock->setMinimumWidth(250);
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* panel = new QWidget(dock);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(8);

    auto* unitRow = new QWidget(panel);
    auto* unitLayout = new QHBoxLayout(unitRow);
    unitLayout->setContentsMargins(0, 0, 0, 0);
    unitLayout->addWidget(new QLabel(QStringLiteral("Единицы"), unitRow));
    units_ = new QComboBox(unitRow);
    units_->addItems({QStringLiteral("мм"), QStringLiteral("см"), QStringLiteral("дюйм")});
    unitLayout->addWidget(units_, 1);
    layout->addWidget(unitRow);

    auto* variablesGroup = new QGroupBox(QStringLiteral("Параметры проекта"), panel);
    auto* variablesLayout = new QVBoxLayout(variablesGroup);
    variablesLayout->setContentsMargins(8, 10, 8, 8);
    variablesLayout->setSpacing(5);
    variablesTable_ = new QTableWidget(0, 2, variablesGroup);
    variablesTable_->setHorizontalHeaderLabels({QStringLiteral("Имя"), QStringLiteral("Формула")});
    variablesTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    variablesTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    variablesTable_->verticalHeader()->setVisible(false);
    variablesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    variablesTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    variablesTable_->setMinimumHeight(74);
    variablesTable_->setMaximumHeight(132);
    variablesTable_->setToolTip(QStringLiteral("Формулы: +, -, *, /, ^, скобки, sin/cos/sqrt/min/max. Пример: L = 120, W = L/2."));
    variablesLayout->addWidget(variablesTable_);
    auto* variableButtons = new QWidget(variablesGroup);
    auto* variableButtonsLayout = new QHBoxLayout(variableButtons);
    variableButtonsLayout->setContentsMargins(0, 0, 0, 0);
    auto* addVariableButton = new QPushButton(QStringLiteral("＋ Переменная"), variableButtons);
    auto* removeVariableButton = new QPushButton(QStringLiteral("− Удалить"), variableButtons);
    variableButtonsLayout->addWidget(addVariableButton);
    variableButtonsLayout->addWidget(removeVariableButton);
    variablesLayout->addWidget(variableButtons);
    variablesErrorLabel_ = new QLabel(variablesGroup);
    variablesErrorLabel_->setObjectName(QStringLiteral("errorLabel"));
    variablesErrorLabel_->setWordWrap(true);
    variablesLayout->addWidget(variablesErrorLabel_);
    layout->addWidget(variablesGroup);

    auto* objectTitle = new QLabel(QStringLiteral("ОБЪЕКТ"), panel);
    objectTitle->setObjectName(QStringLiteral("objectSectionTitle"));
    layout->addWidget(objectTitle);
    objectName_ = new QLineEdit(panel);
    objectName_->setPlaceholderText(QStringLiteral("Выберите объект"));
    layout->addWidget(objectName_);

    auto* positionGroup = new QGroupBox(QStringLiteral("Положение"), panel);
    auto* positionLayout = new QVBoxLayout(positionGroup);
    positionFields_[0] = nullptr;
    positionLayout->addWidget(xyzRow({QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")},
                                     positionFields_, positionGroup, -1000000, 1000000, 3, 1));
    layout->addWidget(positionGroup);

    auto* rotationGroup = new QGroupBox(QStringLiteral("Вращение · градусы"), panel);
    auto* rotationLayout = new QVBoxLayout(rotationGroup);
    rotationLayout->addWidget(xyzRow({QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")},
                                     rotationFields_, rotationGroup, -36000, 36000, 2, 1));
    layout->addWidget(rotationGroup);

    auto* scaleGroup = new QGroupBox(QStringLiteral("Масштаб"), panel);
    auto* scaleLayout = new QVBoxLayout(scaleGroup);
    scaleLayout->addWidget(xyzRow({QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")},
                                  scaleFields_, scaleGroup, 0.001, 100000, 3, 0.1));
    layout->addWidget(scaleGroup);

    primitiveGroup_ = new QGroupBox(QStringLiteral("Параметры фичи · мм"), panel);
    auto* primitiveLayout = new QFormLayout(primitiveGroup_);
    for (int i = 0; i < 3; ++i) {
        primitiveLabels_[static_cast<std::size_t>(i)] = new QLabel(primitiveGroup_);
        primitiveFields_[static_cast<std::size_t>(i)] = new QLineEdit(primitiveGroup_);
        primitiveFields_[static_cast<std::size_t>(i)]->setPlaceholderText(QStringLiteral("число или формула"));
        primitiveLayout->addRow(primitiveLabels_[static_cast<std::size_t>(i)], primitiveFields_[static_cast<std::size_t>(i)]);
    }
    primitiveGroup_->setVisible(false);
    layout->addWidget(primitiveGroup_);

    sketchConstraintsLabel_ = new QLabel(panel);
    sketchConstraintsLabel_->setObjectName(QStringLiteral("hintLabel"));
    sketchConstraintsLabel_->setWordWrap(true);
    sketchConstraintsLabel_->setVisible(false);
    layout->addWidget(sketchConstraintsLabel_);

    editExtrudeButton_ = new QPushButton(QStringLiteral("Предпросмотр / изменить выдавливание"), panel);
    editExtrudeButton_->setVisible(false);
    layout->addWidget(editExtrudeButton_);

    extrudeGroup_ = new QGroupBox(QStringLiteral("Выдавливание · предпросмотр"), panel);
    auto* extrudeLayout = new QVBoxLayout(extrudeGroup_);
    auto* extrudeForm = new QFormLayout;
    extrudeLengthField_ = new QLineEdit(extrudeGroup_);
    extrudeLengthField_->setPlaceholderText(QStringLiteral("например, L/2"));
    extrudeForm->addRow(QStringLiteral("Длина · мм"), extrudeLengthField_);
    extrudeLayout->addLayout(extrudeForm);
    symmetricExtrudeCheck_ = new QCheckBox(QStringLiteral("Симметрично относительно эскиза"), extrudeGroup_);
    extrudeLayout->addWidget(symmetricExtrudeCheck_);
    extrudeStatusLabel_ = new QLabel(extrudeGroup_);
    extrudeStatusLabel_->setObjectName(QStringLiteral("extrudeStatusLabel"));
    extrudeStatusLabel_->setWordWrap(true);
    extrudeLayout->addWidget(extrudeStatusLabel_);
    auto* extrudeButtons = new QWidget(extrudeGroup_);
    auto* extrudeButtonsLayout = new QHBoxLayout(extrudeButtons);
    extrudeButtonsLayout->setContentsMargins(0, 0, 0, 0);
    extrudeCommitButton_ = new QPushButton(QStringLiteral("✓ Применить"), extrudeButtons);
    extrudeCommitButton_->setObjectName(QStringLiteral("accentButton"));
    auto* extrudeCancelButton = new QPushButton(QStringLiteral("Отмена"), extrudeButtons);
    extrudeButtonsLayout->addWidget(extrudeCommitButton_);
    extrudeButtonsLayout->addWidget(extrudeCancelButton);
    extrudeLayout->addWidget(extrudeButtons);
    extrudeGroup_->setVisible(false);
    layout->addWidget(extrudeGroup_);

    auto* geometryGroup = new QGroupBox(QStringLiteral("Геометрия и материал"), panel);
    auto* geometryLayout = new QFormLayout(geometryGroup);
    material_ = new QComboBox(geometryGroup);
    for (const CadMaterial& entry : cadMaterials())
        material_->addItem(entry.name, entry.id);
    geometryLayout->addRow(QStringLiteral("Материал"), material_);
    auto makeValueLabel = [geometryGroup]() {
        auto* value = new QLabel(QStringLiteral("—"), geometryGroup);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return value;
    };
    areaValue_ = makeValueLabel();
    volumeValue_ = makeValueLabel();
    boundsValue_ = makeValueLabel();
    densityValue_ = makeValueLabel();
    massValue_ = makeValueLabel();
    geometryLayout->addRow(QStringLiteral("Площадь"), areaValue_);
    geometryLayout->addRow(QStringLiteral("Объём"), volumeValue_);
    geometryLayout->addRow(QStringLiteral("Габариты"), boundsValue_);
    geometryLayout->addRow(QStringLiteral("Плотность"), densityValue_);
    geometryLayout->addRow(QStringLiteral("Масса"), massValue_);
    layout->addWidget(geometryGroup);

    colorButton_ = new QPushButton(QStringLiteral("Цвет поверхности"), panel);
    layout->addWidget(colorButton_);
    layout->addStretch(1);

    auto* note = new QLabel(QStringLiteral("STEP / IGES сохраняют обменную CAD-геометрию. STL предназначен для Aeros Engine и 3D-печати."), panel);
    note->setObjectName(QStringLiteral("hintLabel"));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* scrollArea = new QScrollArea(dock);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(panel);
    dock->setWidget(scrollArea);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->hide();

    connect(objectName_, &QLineEdit::editingFinished, this, [this] {
        const int index = selectedIndex();
        if (index < 0)
            return;
        const QString name = objectName_->text().trimmed();
        if (name.isEmpty())
            return;
        document_.objects[static_cast<std::size_t>(index)].name = name;
        updateTree();
        pushHistory();
    });
    connect(units_, &QComboBox::currentTextChanged, this, [this](const QString& unit) {
        if (syncingInspector_)
            return;
        document_.units = unit;
        updateInspector();
        pushHistory();
    });
    connect(material_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int itemIndex) {
        const int index = selectedIndex();
        if (syncingInspector_ || index < 0 || itemIndex < 0)
            return;
        document_.objects[static_cast<std::size_t>(index)].materialId = material_->itemData(itemIndex).toString();
        updateInspector();
        pushHistory();
    });
    connect(variablesTable_, &QTableWidget::cellChanged, this, [this] {
        if (!populatingVariables_)
            applyVariablesFromTable();
    });
    connect(addVariableButton, &QPushButton::clicked, this, [this] {
        QSet<QString> names;
        for (int row = 0; row < variablesTable_->rowCount(); ++row) {
            if (QTableWidgetItem* item = variablesTable_->item(row, 0))
                names.insert(item->text().trimmed());
        }
        QString name;
        for (int suffix = 1; suffix < 10000; ++suffix) {
            name = QStringLiteral("Param%1").arg(suffix);
            if (!names.contains(name))
                break;
        }
        populatingVariables_ = true;
        const int row = variablesTable_->rowCount();
        variablesTable_->insertRow(row);
        variablesTable_->setItem(row, 0, new QTableWidgetItem(name));
        variablesTable_->setItem(row, 1, new QTableWidgetItem(QStringLiteral("1")));
        variablesTable_->setCurrentCell(row, 0);
        populatingVariables_ = false;
        applyVariablesFromTable();
    });
    connect(removeVariableButton, &QPushButton::clicked, this, [this] {
        const int row = variablesTable_->currentRow();
        if (row < 0)
            return;
        populatingVariables_ = true;
        variablesTable_->removeRow(row);
        populatingVariables_ = false;
        applyVariablesFromTable();
    });
    for (QLineEdit* field : primitiveFields_)
        connect(field, &QLineEdit::editingFinished, this, [this] {
            if (!syncingInspector_)
                editPrimitiveParameters();
        });
    connect(extrudeLengthField_, &QLineEdit::textChanged, this, [this] { updateExtrudePreview(); });
    connect(symmetricExtrudeCheck_, &QCheckBox::toggled, this, [this] { updateExtrudePreview(); });
    connect(extrudeCommitButton_, &QPushButton::clicked, this, [this] { confirmExtrude(); });
    connect(extrudeCancelButton, &QPushButton::clicked, this, [this] { cancelExtrude(); });
    connect(editExtrudeButton_, &QPushButton::clicked, this, [this] { beginExtrudeEditForSelectedFeature(); });
    for (int axis = 0; axis < 3; ++axis) {
        connect(positionFields_[static_cast<std::size_t>(axis)], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, axis](double value) {
            updateObjectTransform(axis, value, 0);
        });
        connect(rotationFields_[static_cast<std::size_t>(axis)], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, axis](double value) {
            updateObjectTransform(axis, value, 1);
        });
        connect(scaleFields_[static_cast<std::size_t>(axis)], qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, axis](double value) {
            updateObjectTransform(axis, value, 2);
        });
        connect(positionFields_[static_cast<std::size_t>(axis)], &QDoubleSpinBox::editingFinished, this, [this] { if (!syncingInspector_) pushHistory(); });
        connect(rotationFields_[static_cast<std::size_t>(axis)], &QDoubleSpinBox::editingFinished, this, [this] { if (!syncingInspector_) pushHistory(); });
        connect(scaleFields_[static_cast<std::size_t>(axis)], &QDoubleSpinBox::editingFinished, this, [this] { if (!syncingInspector_) pushHistory(); });
    }
    connect(colorButton_, &QPushButton::clicked, this, [this] {
        const int index = selectedIndex();
        if (index < 0)
            return;
        CadObject& object = document_.objects[static_cast<std::size_t>(index)];
        const QColor color = QColorDialog::getColor(object.color, this, QStringLiteral("Цвет объекта"));
        if (!color.isValid())
            return;
        object.color = color;
        updateInspector();
        viewport_->update();
        pushHistory();
    });
}

void MainWindow::createMenusAndRibbon()
{
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("Файл"));
    auto* editMenu = menuBar()->addMenu(QStringLiteral("Правка"));
    auto* selectionMenu = menuBar()->addMenu(QStringLiteral("Выделить"));
    auto* viewMenu = menuBar()->addMenu(QStringLiteral("Вид"));
    auto* sketchMenu = menuBar()->addMenu(QStringLiteral("Эскиз"));
    auto* modelingMenu = menuBar()->addMenu(QStringLiteral("Моделирование"));
    auto* appearanceMenu = menuBar()->addMenu(QStringLiteral("Оформление"));
    auto* diagnosticsMenu = menuBar()->addMenu(QStringLiteral("Диагностика"));
    auto* managementMenu = menuBar()->addMenu(QStringLiteral("Управление"));
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("Настройка"));
    auto* applicationsMenu = menuBar()->addMenu(QStringLiteral("Приложения"));
    auto* windowMenu = menuBar()->addMenu(QStringLiteral("Окно"));
    auto* helpMenu = menuBar()->addMenu(QStringLiteral("Справка"));

    auto* toolBar = addToolBar(QStringLiteral("Лента команд"));
    toolBar->setObjectName(QStringLiteral("ribbonHost"));
    toolBar->setMovable(false);
    toolBar->setFloatable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    toolBar->setIconSize(QSize(24, 24));
    ribbonTabs_ = new QTabWidget(toolBar);
    ribbonTabs_->setObjectName(QStringLiteral("commandRibbon"));
    ribbonTabs_->setDocumentMode(true);
    ribbonTabs_->setTabPosition(QTabWidget::North);
    ribbonTabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ribbonTabs_->setMinimumHeight(98);
    toolBar->addWidget(ribbonTabs_);

    QAction* newAction = fileMenu->addAction(QStringLiteral("Новый проект"));
    newAction->setShortcut(QKeySequence::New);
    fileMenu->addSeparator();
    QAction* openAction = fileMenu->addAction(QStringLiteral("Открыть проект…"));
    openAction->setShortcut(QKeySequence::Open);
    QAction* importAction = fileMenu->addAction(QStringLiteral("Импорт CAD-файла…"));
    importAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));
    QAction* saveAction = fileMenu->addAction(QStringLiteral("Сохранить проект"));
    saveAction->setShortcut(QKeySequence::Save);
    QAction* saveAsAction = fileMenu->addAction(QStringLiteral("Сохранить проект как…"));
    saveAsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    fileMenu->addSeparator();
    auto* exportMenu = fileMenu->addMenu(QStringLiteral("Экспортировать"));
    QAction* exportStlAction = exportMenu->addAction(QStringLiteral("STL · Aeros Engine / 3D-печать…"));
    QAction* exportStepAction = exportMenu->addAction(QStringLiteral("STEP AP214/AP242…"));
    QAction* exportIgesAction = exportMenu->addAction(QStringLiteral("IGES…"));
    QAction* exportObjAction = exportMenu->addAction(QStringLiteral("OBJ-сетка…"));
    QAction* exportBrepAction = exportMenu->addAction(QStringLiteral("BREP-геометрия…"));
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("Выход"));
    quitAction->setShortcut(QKeySequence::Quit);

    undoAction_ = editMenu->addAction(QStringLiteral("Отменить"));
    undoAction_->setShortcut(QKeySequence::Undo);
    redoAction_ = editMenu->addAction(QStringLiteral("Повторить"));
    redoAction_->setShortcut(QKeySequence::Redo);
    editMenu->addSeparator();
    QAction* duplicateAction = editMenu->addAction(QStringLiteral("Дублировать объект"));
    duplicateAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    QAction* deleteAction = editMenu->addAction(QStringLiteral("Удалить объект"));
    deleteAction->setShortcut(QKeySequence::Delete);

    auto* toolGroup = new QActionGroup(this);
    toolGroup->setExclusive(true);
    const auto addToolAction = [this, toolGroup](const QString& name,
                                                 const QString& iconName,
                                                 const QString& ribbonText,
                                                 const QKeySequence& shortcut,
                                                 CadViewport::Tool tool) {
        auto* action = new QAction(makeRibbonIcon(iconName, darkTheme_), name, this);
        action->setProperty("ribbonIconName", iconName);
        action->setCheckable(true);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::ApplicationShortcut);
        action->setProperty("ribbonText", ribbonText);
        addAction(action);
        toolGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool] { setTool(tool); });
        return action;
    };
    QAction* selectAction = addToolAction(QStringLiteral("Выбор"), QStringLiteral("select"), QStringLiteral("Выбор"),
                                          QKeySequence(QStringLiteral("Q")), CadViewport::Tool::Select);
    QAction* moveAction = addToolAction(QStringLiteral("Переместить"), QStringLiteral("move"), QStringLiteral("Перемещение"),
                                        QKeySequence(QStringLiteral("W")), CadViewport::Tool::Move);
    QAction* rotateAction = addToolAction(QStringLiteral("Вращать"), QStringLiteral("rotate"), QStringLiteral("Вращение"),
                                          QKeySequence(QStringLiteral("E")), CadViewport::Tool::Rotate);
    QAction* scaleAction = addToolAction(QStringLiteral("Масштаб"), QStringLiteral("scale"), QStringLiteral("Масштаб"),
                                         QKeySequence(QStringLiteral("R")), CadViewport::Tool::Scale);
    QAction* measureAction = addToolAction(QStringLiteral("Измерить"), QStringLiteral("measure"), QStringLiteral("Измерение"),
                                           QKeySequence(QStringLiteral("M")), CadViewport::Tool::Measure);
    selectAction->setChecked(true);
    selectionMenu->addAction(selectAction);
    managementMenu->addAction(moveAction);
    managementMenu->addAction(rotateAction);
    managementMenu->addAction(scaleAction);
    diagnosticsMenu->addAction(measureAction);
    undoAction_->setEnabled(false);
    redoAction_->setEnabled(false);

    QAction* boxAction = modelingMenu->addAction(QStringLiteral("Бокс"));
    QAction* cylinderAction = modelingMenu->addAction(QStringLiteral("Цилиндр"));
    QAction* sphereAction = modelingMenu->addAction(QStringLiteral("Сфера"));
    QAction* coneAction = modelingMenu->addAction(QStringLiteral("Конус"));
    QAction* torusAction = modelingMenu->addAction(QStringLiteral("Тор"));
    QAction* rectangleSketchAction = sketchMenu->addAction(QStringLiteral("Прямоугольник"));
    QAction* circleSketchAction = sketchMenu->addAction(QStringLiteral("Окружность"));
    QAction* extrudeAction = modelingMenu->addAction(QStringLiteral("Выдавливание…"));
    modelingMenu->addSeparator();
    QAction* filletAction = modelingMenu->addAction(QStringLiteral("Скруглить все рёбра…"));
    QAction* chamferAction = modelingMenu->addAction(QStringLiteral("Снять фаску со всех рёбер…"));
    auto* booleanMenu = modelingMenu->addMenu(QStringLiteral("Булевы операции"));
    QAction* booleanUnionAction = booleanMenu->addAction(QStringLiteral("Объединение тел…"));
    QAction* booleanCutAction = booleanMenu->addAction(QStringLiteral("Вычитание тел…"));
    QAction* booleanCommonAction = booleanMenu->addAction(QStringLiteral("Пересечение тел…"));
    auto* patternMenu = modelingMenu->addMenu(QStringLiteral("Линейный массив"));
    QAction* patternXAction = patternMenu->addAction(QStringLiteral("По оси X…"));
    QAction* patternYAction = patternMenu->addAction(QStringLiteral("По оси Y…"));
    QAction* patternZAction = patternMenu->addAction(QStringLiteral("По оси Z…"));
    auto* mirrorMenu = modelingMenu->addMenu(QStringLiteral("Зеркальная копия"));
    QAction* mirrorXYAction = mirrorMenu->addAction(QStringLiteral("Относительно XY"));
    QAction* mirrorYZAction = mirrorMenu->addAction(QStringLiteral("Относительно YZ"));
    QAction* mirrorZXAction = mirrorMenu->addAction(QStringLiteral("Относительно ZX"));

    QAction* fitAction = viewMenu->addAction(QStringLiteral("Вписать модель"));
    fitAction->setShortcut(QKeySequence(QStringLiteral("F")));
    QAction* isoAction = viewMenu->addAction(QStringLiteral("Изометрия"));
    QAction* topAction = viewMenu->addAction(QStringLiteral("Сверху"));
    QAction* frontAction = viewMenu->addAction(QStringLiteral("Спереди"));
    QAction* rightAction = viewMenu->addAction(QStringLiteral("Справа"));
    QAction* gridAction = settingsMenu->addAction(QStringLiteral("Показать сетку"));
    gridAction->setCheckable(true);
    gridAction->setChecked(false);

    QAction* objectAppearanceAction = appearanceMenu->addAction(QStringLiteral("Цвет и материал объекта…"));
    QAction* projectSettingsAction = settingsMenu->addAction(QStringLiteral("Параметры проекта…"));
    settingsMenu->addSeparator();
    auto* themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    darkThemeAction_ = settingsMenu->addAction(QStringLiteral("Тёмная тема"));
    darkThemeAction_->setCheckable(true);
    themeGroup->addAction(darkThemeAction_);
    lightThemeAction_ = settingsMenu->addAction(QStringLiteral("Светлая тема"));
    lightThemeAction_->setCheckable(true);
    themeGroup->addAction(lightThemeAction_);
    QAction* projectDockAction = projectDock_->toggleViewAction();
    projectDockAction->setText(QStringLiteral("Дерево модели"));
    QAction* propertiesAction = inspectorDock_->toggleViewAction();
    propertiesAction->setText(QStringLiteral("Панель свойств"));
    windowMenu->addAction(projectDockAction);
    windowMenu->addAction(propertiesAction);

    QAction* updateAction = applicationsMenu->addAction(QStringLiteral("Проверить обновления…"));
    QAction* compatibilityAction = helpMenu->addAction(QStringLiteral("Форматы и совместимость CAD"));
    helpMenu->addSeparator();
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("О программе AoSCAD Studio"));

    const auto setRibbonIcon = [this](QAction* action, const QString& iconName, const QString& ribbonText) {
        action->setIcon(makeRibbonIcon(iconName, darkTheme_));
        action->setProperty("ribbonIconName", iconName);
        action->setProperty("ribbonText", ribbonText);
    };
    setRibbonIcon(newAction, QStringLiteral("new"), QStringLiteral("Новый"));
    setRibbonIcon(openAction, QStringLiteral("open"), QStringLiteral("Открыть"));
    setRibbonIcon(importAction, QStringLiteral("import"), QStringLiteral("Импорт"));
    setRibbonIcon(saveAction, QStringLiteral("save"), QStringLiteral("Сохранить"));
    setRibbonIcon(undoAction_, QStringLiteral("undo"), QStringLiteral("Отменить"));
    setRibbonIcon(redoAction_, QStringLiteral("redo"), QStringLiteral("Повторить"));
    setRibbonIcon(duplicateAction, QStringLiteral("duplicate"), QStringLiteral("Дублировать"));
    setRibbonIcon(deleteAction, QStringLiteral("delete"), QStringLiteral("Удалить"));
    setRibbonIcon(boxAction, QStringLiteral("box"), QStringLiteral("Бокс"));
    setRibbonIcon(cylinderAction, QStringLiteral("cylinder"), QStringLiteral("Цилиндр"));
    setRibbonIcon(sphereAction, QStringLiteral("sphere"), QStringLiteral("Сфера"));
    setRibbonIcon(coneAction, QStringLiteral("cone"), QStringLiteral("Конус"));
    setRibbonIcon(torusAction, QStringLiteral("torus"), QStringLiteral("Тор"));
    setRibbonIcon(rectangleSketchAction, QStringLiteral("rectangle"), QStringLiteral("Прямоугольник"));
    setRibbonIcon(circleSketchAction, QStringLiteral("circle"), QStringLiteral("Окружность"));
    setRibbonIcon(extrudeAction, QStringLiteral("extrude"), QStringLiteral("Выдавить"));
    setRibbonIcon(filletAction, QStringLiteral("fillet"), QStringLiteral("Скруглить"));
    setRibbonIcon(chamferAction, QStringLiteral("chamfer"), QStringLiteral("Фаска"));
    setRibbonIcon(booleanUnionAction, QStringLiteral("union"), QStringLiteral("Объединить"));
    setRibbonIcon(booleanCutAction, QStringLiteral("cut"), QStringLiteral("Вычесть"));
    setRibbonIcon(booleanCommonAction, QStringLiteral("common"), QStringLiteral("Пересечь"));
    setRibbonIcon(patternXAction, QStringLiteral("pattern"), QStringLiteral("Массив X"));
    setRibbonIcon(mirrorXYAction, QStringLiteral("mirror"), QStringLiteral("Зеркало XY"));
    setRibbonIcon(fitAction, QStringLiteral("fit"), QStringLiteral("Вписать"));
    setRibbonIcon(gridAction, QStringLiteral("grid"), QStringLiteral("Сетка"));
    setRibbonIcon(isoAction, QStringLiteral("iso"), QStringLiteral("Изометрия"));
    setRibbonIcon(topAction, QStringLiteral("top"), QStringLiteral("Сверху"));
    setRibbonIcon(frontAction, QStringLiteral("front"), QStringLiteral("Спереди"));
    setRibbonIcon(rightAction, QStringLiteral("right"), QStringLiteral("Справа"));

    const auto createRibbonPage = [this](const QString& title, QHBoxLayout*& pageLayout) {
        auto* page = new QWidget(ribbonTabs_);
        pageLayout = new QHBoxLayout(page);
        pageLayout->setContentsMargins(6, 2, 6, 2);
        pageLayout->setSpacing(5);
        ribbonTabs_->addTab(page, title);
        return page;
    };
    const auto addRibbonGroup = [](QWidget* page, QHBoxLayout* pageLayout, const QString& title,
                                   std::initializer_list<QAction*> actions) {
        auto* group = new QWidget(page);
        auto* groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(0);
        auto* buttons = new QHBoxLayout;
        buttons->setContentsMargins(0, 0, 0, 0);
        buttons->setSpacing(2);
        for (QAction* action : actions) {
            auto* button = new QToolButton(group);
            button->setDefaultAction(action);
            const QString ribbonText = action->property("ribbonText").toString();
            if (!ribbonText.isEmpty())
                button->setText(ribbonText);
            button->setObjectName(QStringLiteral("ribbonButton"));
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            button->setIconSize(QSize(21, 21));
            button->setMinimumSize(66, 54);
            button->setMaximumWidth(90);
            buttons->addWidget(button);
        }
        auto* caption = new QLabel(title, group);
        caption->setObjectName(QStringLiteral("ribbonGroupCaption"));
        caption->setAlignment(Qt::AlignCenter);
        groupLayout->addLayout(buttons, 1);
        groupLayout->addWidget(caption);
        pageLayout->addWidget(group);
    };
    const auto addRibbonSeparator = [](QWidget* page, QHBoxLayout* pageLayout) {
        auto* separator = new QFrame(page);
        separator->setObjectName(QStringLiteral("ribbonSeparator"));
        separator->setFrameShape(QFrame::VLine);
        separator->setFrameShadow(QFrame::Plain);
        pageLayout->addWidget(separator);
    };

    QHBoxLayout* modelLayout = nullptr;
    QWidget* modelPage = createRibbonPage(QStringLiteral("Модель"), modelLayout);
    addRibbonGroup(modelPage, modelLayout, QStringLiteral("Проект"), {newAction, openAction, importAction, saveAction});
    addRibbonSeparator(modelPage, modelLayout);
    addRibbonGroup(modelPage, modelLayout, QStringLiteral("История"), {undoAction_, redoAction_});
    addRibbonSeparator(modelPage, modelLayout);
    addRibbonGroup(modelPage, modelLayout, QStringLiteral("Тело"),
                   {boxAction, cylinderAction, sphereAction, coneAction, torusAction});
    addRibbonSeparator(modelPage, modelLayout);
    addRibbonGroup(modelPage, modelLayout, QStringLiteral("Редактирование"), {duplicateAction, deleteAction});
    modelLayout->addStretch(1);

    QHBoxLayout* sketchLayout = nullptr;
    QWidget* sketchPage = createRibbonPage(QStringLiteral("Эскиз"), sketchLayout);
    addRibbonGroup(sketchPage, sketchLayout, QStringLiteral("Профили"), {rectangleSketchAction, circleSketchAction});
    addRibbonSeparator(sketchPage, sketchLayout);
    addRibbonGroup(sketchPage, sketchLayout, QStringLiteral("Выдавливание"), {extrudeAction});
    sketchLayout->addStretch(1);

    QHBoxLayout* transformLayout = nullptr;
    QWidget* transformPage = createRibbonPage(QStringLiteral("Преобразования"), transformLayout);
    addRibbonGroup(transformPage, transformLayout, QStringLiteral("Инструменты"),
                   {selectAction, moveAction, rotateAction, scaleAction, measureAction});
    addRibbonSeparator(transformPage, transformLayout);
    addRibbonGroup(transformPage, transformLayout, QStringLiteral("Обработка рёбер"), {filletAction, chamferAction});
    addRibbonSeparator(transformPage, transformLayout);
    addRibbonGroup(transformPage, transformLayout, QStringLiteral("Булевы операции"),
                   {booleanUnionAction, booleanCutAction, booleanCommonAction});
    addRibbonSeparator(transformPage, transformLayout);
    addRibbonGroup(transformPage, transformLayout, QStringLiteral("Копирование"), {patternXAction, mirrorXYAction});
    transformLayout->addStretch(1);

    QHBoxLayout* viewLayout = nullptr;
    QWidget* viewPage = createRibbonPage(QStringLiteral("Вид"), viewLayout);
    addRibbonGroup(viewPage, viewLayout, QStringLiteral("Камера"), {fitAction, gridAction});
    addRibbonSeparator(viewPage, viewLayout);
    addRibbonGroup(viewPage, viewLayout, QStringLiteral("Стандартные виды"), {isoAction, topAction, frontAction, rightAction});
    viewLayout->addStretch(1);

    connect(newAction, &QAction::triggered, this, [this] { newProject(); });
    connect(openAction, &QAction::triggered, this, [this] { openProject(); });
    connect(importAction, &QAction::triggered, this, [this] { importFiles(); });
    connect(saveAction, &QAction::triggered, this, [this] { saveProject(); });
    connect(saveAsAction, &QAction::triggered, this, [this] { saveProject(true); });
    connect(quitAction, &QAction::triggered, this, [this] { close(); });
    connect(exportStlAction, &QAction::triggered, this, [this] { exportTo(QStringLiteral("stl")); });
    connect(exportStepAction, &QAction::triggered, this, [this] { exportTo(QStringLiteral("step")); });
    connect(exportIgesAction, &QAction::triggered, this, [this] { exportTo(QStringLiteral("iges")); });
    connect(exportObjAction, &QAction::triggered, this, [this] { exportTo(QStringLiteral("obj")); });
    connect(exportBrepAction, &QAction::triggered, this, [this] { exportTo(QStringLiteral("brep")); });
    connect(undoAction_, &QAction::triggered, this, [this] { restoreHistory(historyIndex_ - 1); });
    connect(redoAction_, &QAction::triggered, this, [this] { restoreHistory(historyIndex_ + 1); });
    connect(duplicateAction, &QAction::triggered, this, [this] { duplicateSelected(); });
    connect(deleteAction, &QAction::triggered, this, [this] { deleteSelected(); });
    connect(boxAction, &QAction::triggered, this, [this] { addPrimitive(QStringLiteral("box")); });
    connect(cylinderAction, &QAction::triggered, this, [this] { addPrimitive(QStringLiteral("cylinder")); });
    connect(sphereAction, &QAction::triggered, this, [this] { addPrimitive(QStringLiteral("sphere")); });
    connect(coneAction, &QAction::triggered, this, [this] { addPrimitive(QStringLiteral("cone")); });
    connect(torusAction, &QAction::triggered, this, [this] { addPrimitive(QStringLiteral("torus")); });
    connect(rectangleSketchAction, &QAction::triggered, this, [this] { startSketch(CadViewport::SketchTool::Rectangle); });
    connect(circleSketchAction, &QAction::triggered, this, [this] { startSketch(CadViewport::SketchTool::Circle); });
    connect(extrudeAction, &QAction::triggered, this, [this] { beginExtrudeForSelectedSketch(); });
    connect(filletAction, &QAction::triggered, this, [this] { createEdgeTreatmentFeature(false); });
    connect(chamferAction, &QAction::triggered, this, [this] { createEdgeTreatmentFeature(true); });
    connect(booleanUnionAction, &QAction::triggered, this, [this] { createBooleanFeature(QStringLiteral("union")); });
    connect(booleanCutAction, &QAction::triggered, this, [this] { createBooleanFeature(QStringLiteral("cut")); });
    connect(booleanCommonAction, &QAction::triggered, this, [this] { createBooleanFeature(QStringLiteral("common")); });
    connect(patternXAction, &QAction::triggered, this, [this] { createLinearPatternFeature(QStringLiteral("X")); });
    connect(patternYAction, &QAction::triggered, this, [this] { createLinearPatternFeature(QStringLiteral("Y")); });
    connect(patternZAction, &QAction::triggered, this, [this] { createLinearPatternFeature(QStringLiteral("Z")); });
    connect(mirrorXYAction, &QAction::triggered, this, [this] { createMirrorFeature(QStringLiteral("XY")); });
    connect(mirrorYZAction, &QAction::triggered, this, [this] { createMirrorFeature(QStringLiteral("YZ")); });
    connect(mirrorZXAction, &QAction::triggered, this, [this] { createMirrorFeature(QStringLiteral("ZX")); });
    connect(fitAction, &QAction::triggered, viewport_, &CadViewport::fitAll);
    connect(gridAction, &QAction::toggled, viewport_, &CadViewport::setGridVisible);
    connect(darkThemeAction_, &QAction::triggered, this, [this] { applyTheme(true); });
    connect(lightThemeAction_, &QAction::triggered, this, [this] { applyTheme(false); });
    connect(isoAction, &QAction::triggered, this, [this] { viewport_->setStandardView(QStringLiteral("iso")); });
    connect(topAction, &QAction::triggered, this, [this] { viewport_->setStandardView(QStringLiteral("top")); });
    connect(frontAction, &QAction::triggered, this, [this] { viewport_->setStandardView(QStringLiteral("front")); });
    connect(rightAction, &QAction::triggered, this, [this] { viewport_->setStandardView(QStringLiteral("right")); });
    const auto showInspector = [this] {
        inspectorDock_->show();
        inspectorDock_->raise();
    };
    connect(objectAppearanceAction, &QAction::triggered, this, showInspector);
    connect(projectSettingsAction, &QAction::triggered, this, showInspector);
    connect(updateAction, &QAction::triggered, this, [this] { launchUpdater(); });
    connect(compatibilityAction, &QAction::triggered, this, [this] { showCompatibilityHelp(); });
    connect(aboutAction, &QAction::triggered, this, [this] { showAbout(); });
}

void MainWindow::connectActions()
{
    connect(viewport_, &CadViewport::sketchFinished, this, [this](const QString& type, const QVector2D& origin, const QVector2D& size) {
        if (type.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Эскиз отменён или слишком мал: увеличьте размер и повторите."), 3500);
            return;
        }
        createSketch(type, origin, size);
    });
    connect(viewport_, &CadViewport::selectionChanged, this, [this](int index) {
        viewport_->setActiveReferencePlane(CadViewport::ReferencePlane::None);
        if (tree_) {
            const QSignalBlocker blocker(tree_);
            if (index >= 0 && static_cast<std::size_t>(index) < document_.objects.size()) {
                for (int row = 1; row < tree_->topLevelItemCount(); ++row) {
                    QTreeWidgetItem* item = tree_->topLevelItem(row);
                    if (item->data(0, kTreeNodeTypeRole).toInt() == static_cast<int>(TreeNodeType::Object) &&
                        item->data(0, Qt::UserRole).toInt() == index) {
                        tree_->setCurrentItem(item);
                        break;
                    }
                }
            } else {
                tree_->clearSelection();
                tree_->setCurrentItem(nullptr);
            }
        }
        updateInspector();
    });
    connect(viewport_, &CadViewport::objectChanged, this, [this](bool committed) {
        if (committed) {
            const int selectedBeforeRecompute = selectedIndex();
            try {
                CadDocument candidate = document_;
                CadIO::recomputeParametricFeatures(candidate);
                document_ = std::move(candidate);
                viewport_->refreshGeometry();
                if (!pendingExtrudeSketchId_.isEmpty())
                    updateExtrudePreview();
            } catch (const Standard_Failure& error) {
                if (historyIndex_ >= 0 && historyIndex_ < static_cast<int>(history_.size()))
                    document_ = history_[static_cast<std::size_t>(historyIndex_)];
                viewport_->setDocument(&document_);
                viewport_->setSelectedObject(selectedBeforeRecompute);
                updateTree();
                updateInspector();
                statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 6000);
                return;
            } catch (const std::exception& error) {
                if (historyIndex_ >= 0 && historyIndex_ < static_cast<int>(history_.size()))
                    document_ = history_[static_cast<std::size_t>(historyIndex_)];
                viewport_->setDocument(&document_);
                viewport_->setSelectedObject(selectedBeforeRecompute);
                updateTree();
                updateInspector();
                statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
                return;
            }
        }
        updateTree();
        updateInspector(committed);
        if (committed)
            pushHistory();
    });
    connect(viewport_, &CadViewport::measurementCompleted, this, [this](double distance) {
        const double factor = unitFactor();
        const QString unit = document_.units;
        statusBar()->showMessage(QStringLiteral("Расстояние: %1 %2").arg(distance * factor, 0, 'f', 2).arg(unit), 5000);
    });
    connect(viewport_, &CadViewport::worldPositionChanged, this, [this](const QVector3D& position) {
        const double factor = unitFactor();
        coordinateLabel_->setText(QStringLiteral("X %1   Y %2   Z %3 %4")
                                      .arg(position.x() * factor, 0, 'f', 2)
                                      .arg(position.y() * factor, 0, 'f', 2)
                                      .arg(position.z() * factor, 0, 'f', 2)
                                      .arg(document_.units));
    });
}

void MainWindow::updateTree()
{
    if (!tree_)
        return;
    const QSignalBlocker blocker(tree_);
    tree_->clear();

    auto* origin = new QTreeWidgetItem(tree_);
    origin->setText(0, QStringLiteral("Начало координат"));
    origin->setData(0, Qt::UserRole, -1);
    origin->setData(0, kTreeNodeTypeRole, static_cast<int>(TreeNodeType::Origin));
    origin->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    const auto addReferencePlane = [origin](const QString& name, CadViewport::ReferencePlane plane) {
        auto* item = new QTreeWidgetItem(origin);
        item->setText(0, name);
        item->setData(0, Qt::UserRole, -1);
        item->setData(0, kTreeNodeTypeRole, static_cast<int>(TreeNodeType::ReferencePlane));
        item->setData(0, kReferencePlaneRole, static_cast<int>(plane));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    };
    addReferencePlane(QStringLiteral("Плоскость XY"), CadViewport::ReferencePlane::XY);
    addReferencePlane(QStringLiteral("Плоскость YZ"), CadViewport::ReferencePlane::YZ);
    addReferencePlane(QStringLiteral("Плоскость ZX"), CadViewport::ReferencePlane::ZX);
    origin->setExpanded(true);

    for (std::size_t i = 0; i < document_.objects.size(); ++i) {
        const CadObject& object = document_.objects[i];
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, object.name);
        item->setData(0, Qt::UserRole, static_cast<int>(i));
        item->setData(0, kTreeNodeTypeRole, static_cast<int>(TreeNodeType::Object));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        item->setCheckState(0, object.visible ? Qt::Checked : Qt::Unchecked);
    }

    if (viewport_->activeReferencePlane() != CadViewport::ReferencePlane::None) {
        for (int row = 0; row < origin->childCount(); ++row) {
            QTreeWidgetItem* item = origin->child(row);
            if (item->data(0, kReferencePlaneRole).toInt() ==
                static_cast<int>(viewport_->activeReferencePlane())) {
                tree_->setCurrentItem(item);
                break;
            }
        }
    } else if (selectedIndex() >= 0) {
        for (int row = 1; row < tree_->topLevelItemCount(); ++row) {
            QTreeWidgetItem* item = tree_->topLevelItem(row);
            if (item->data(0, Qt::UserRole).toInt() == selectedIndex()) {
                tree_->setCurrentItem(item);
                break;
            }
        }
    }
    updateStatus();
}

void MainWindow::updateInspector(bool updateMeasurements)
{
    if (!units_)
        return;
    syncingInspector_ = true;
    {
        const QSignalBlocker blocker(units_);
        units_->setCurrentText(document_.units);
    }
    if (projectName_ && projectName_->text() != document_.name) {
        const QSignalBlocker blocker(projectName_);
        projectName_->setText(document_.name);
    }
    updateVariablesTable();

    const int index = selectedIndex();
    const bool hasObject = index >= 0 && static_cast<std::size_t>(index) < document_.objects.size();
    objectName_->setEnabled(hasObject);
    colorButton_->setEnabled(hasObject);
    material_->setEnabled(hasObject);
    for (QDoubleSpinBox* field : positionFields_) field->setEnabled(hasObject);
    for (QDoubleSpinBox* field : rotationFields_) field->setEnabled(hasObject);
    for (QDoubleSpinBox* field : scaleFields_) field->setEnabled(hasObject);
    for (QLineEdit* field : primitiveFields_) field->setEnabled(hasObject);

    if (hasObject) {
        const CadObject& object = document_.objects[static_cast<std::size_t>(index)];
        const bool transformable = object.featureType != QStringLiteral("sketch");
        for (QDoubleSpinBox* field : positionFields_) field->setEnabled(transformable);
        for (QDoubleSpinBox* field : rotationFields_) field->setEnabled(transformable);
        for (QDoubleSpinBox* field : scaleFields_) field->setEnabled(transformable);
        objectName_->setText(object.name);
        const double factor = unitFactor();
        const float position[]{object.position.x(), object.position.y(), object.position.z()};
        const float rotation[]{object.rotation.x(), object.rotation.y(), object.rotation.z()};
        const float scale[]{object.scale.x(), object.scale.y(), object.scale.z()};
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t i = static_cast<std::size_t>(axis);
            const QSignalBlocker pBlock(positionFields_[i]);
            const QSignalBlocker rBlock(rotationFields_[i]);
            const QSignalBlocker sBlock(scaleFields_[i]);
            positionFields_[i]->setValue(position[axis] * factor);
            positionFields_[i]->setSuffix(QStringLiteral(" %1").arg(document_.units));
            rotationFields_[i]->setValue(rotation[axis]);
            rotationFields_[i]->setSuffix(QStringLiteral("°"));
            scaleFields_[i]->setValue(scale[axis]);
            scaleFields_[i]->setSuffix(QStringLiteral("×"));
        }

        const QStringList parameterNames = primitiveParameterNames(object.primitiveType);
        const bool isExtrude = object.featureType == QStringLiteral("extrude");
        const bool isSketch = object.featureType == QStringLiteral("sketch");
        const bool isParametric = !isExtrude && !parameterNames.isEmpty() &&
                                  parameterNames.size() == static_cast<int>(object.primitiveParameters.size());
        primitiveGroup_->setVisible(isParametric);
        editExtrudeButton_->setVisible(isExtrude && pendingExtrudeTargetId_.isEmpty());
        editExtrudeButton_->setEnabled(isExtrude && !object.suppressed);
        sketchConstraintsLabel_->setVisible(isSketch);
        if (isSketch) {
            sketchConstraintsLabel_->setText(object.primitiveType == QStringLiteral("sketch-circle")
                ? QStringLiteral("Базовые ограничения: центр и радиус заданы; окружность замкнута. Привязка к сетке 5 мм и началу координат. Полноценный solver ограничений — следующий этап.")
                : QStringLiteral("Базовые ограничения: стороны горизонтальны/вертикальны, контур замкнут, начало и размеры параметрические. Привязка к сетке 5 мм и началу координат. Полноценный solver ограничений — следующий этап."));
        } else {
            sketchConstraintsLabel_->clear();
        }
        for (int i = 0; i < 3; ++i) {
            const std::size_t parameterIndex = static_cast<std::size_t>(i);
            const bool visible = isParametric && i < parameterNames.size();
            primitiveLabels_[parameterIndex]->setVisible(visible);
            primitiveFields_[parameterIndex]->setVisible(visible);
            primitiveFields_[parameterIndex]->setEnabled(visible && !object.suppressed);
            const QSignalBlocker parameterBlock(primitiveFields_[parameterIndex]);
            if (visible) {
                primitiveLabels_[parameterIndex]->setText(parameterNames[i]);
                const QString key = QStringLiteral("p%1").arg(i);
                const QString expression = object.parameterExpressions.value(key).trimmed();
                primitiveFields_[parameterIndex]->setText(expression.isEmpty()
                    ? QString::number(object.primitiveParameters[parameterIndex], 'g', 12)
                    : expression);
            } else {
                primitiveFields_[parameterIndex]->clear();
            }
        }
        {
            const QSignalBlocker materialBlock(material_);
            int materialIndex = material_->findData(object.materialId);
            if (materialIndex < 0)
                materialIndex = 0;
            material_->setCurrentIndex(materialIndex);
        }
        colorButton_->setText(object.color.name(QColor::HexRgb).toUpper());
        colorButton_->setStyleSheet(QStringLiteral("background:%1;color:%2;border:1px solid %3;padding:8px;")
                                        .arg(object.color.name())
                                        .arg(object.color.lightness() > 140 ? QStringLiteral("#252525") : QStringLiteral("white"))
                                        .arg(darkTheme_ ? QStringLiteral("#505050") : QStringLiteral("#C8C8C8")));
        if (updateMeasurements)
            updatePhysicalInspector(object);
    } else {
        objectName_->clear();
        primitiveGroup_->setVisible(false);
        editExtrudeButton_->setVisible(false);
        sketchConstraintsLabel_->setVisible(false);
        colorButton_->setText(QStringLiteral("Цвет поверхности"));
        colorButton_->setStyleSheet(QString());
        {
            const QSignalBlocker materialBlock(material_);
            material_->setCurrentIndex(0);
        }
        for (int i = 0; i < 3; ++i) {
            const std::size_t parameterIndex = static_cast<std::size_t>(i);
            primitiveLabels_[parameterIndex]->hide();
            primitiveFields_[parameterIndex]->setEnabled(false);
            const QSignalBlocker parameterBlock(primitiveFields_[parameterIndex]);
            primitiveFields_[parameterIndex]->clear();
        }
        for (QDoubleSpinBox* field : positionFields_) field->setValue(0.0);
        for (QDoubleSpinBox* field : rotationFields_) field->setValue(0.0);
        for (QDoubleSpinBox* field : scaleFields_) field->setValue(1.0);
        areaValue_->setText(QStringLiteral("—"));
        volumeValue_->setText(QStringLiteral("—"));
        boundsValue_->setText(QStringLiteral("—"));
        densityValue_->setText(QStringLiteral("—"));
        massValue_->setText(QStringLiteral("—"));
    }
    syncingInspector_ = false;
}

void MainWindow::updatePhysicalInspector(const CadObject& object)
{
    CadGeometryProperties properties;
    try {
        properties = CadIO::analyzeShape(object.worldShape());
    } catch (const Standard_Failure&) {
        areaValue_->setText(QStringLiteral("—"));
        volumeValue_->setText(QStringLiteral("—"));
        boundsValue_->setText(QStringLiteral("—"));
        densityValue_->setText(QStringLiteral("—"));
        massValue_->setText(QStringLiteral("—"));
        return;
    } catch (const std::exception&) {
        areaValue_->setText(QStringLiteral("—"));
        volumeValue_->setText(QStringLiteral("—"));
        boundsValue_->setText(QStringLiteral("—"));
        densityValue_->setText(QStringLiteral("—"));
        massValue_->setText(QStringLiteral("—"));
        return;
    }

    const double factor = unitFactor();
    const QString unit = document_.units;
    if (properties.surfaceAreaMm2 > 0.0 && std::isfinite(properties.surfaceAreaMm2)) {
        areaValue_->setText(QStringLiteral("%1 %2²").arg(properties.surfaceAreaMm2 * factor * factor, 0, 'g', 6).arg(unit));
    } else {
        areaValue_->setText(QStringLiteral("—"));
    }
    if (properties.hasVolume) {
        volumeValue_->setText(QStringLiteral("%1 %2³").arg(properties.volumeMm3 * factor * factor * factor, 0, 'g', 6).arg(unit));
    } else {
        volumeValue_->setText(QStringLiteral("нет данных"));
    }
    if (properties.hasBounds) {
        boundsValue_->setText(QStringLiteral("%1 × %2 × %3 %4")
                                  .arg(properties.boundsMm.x() * factor, 0, 'g', 5)
                                  .arg(properties.boundsMm.y() * factor, 0, 'g', 5)
                                  .arg(properties.boundsMm.z() * factor, 0, 'g', 5)
                                  .arg(unit));
    } else {
        boundsValue_->setText(QStringLiteral("—"));
    }

    const double density = materialDensityKgPerM3(object.materialId);
    if (density <= 0.0) {
        densityValue_->setText(QStringLiteral("не задана"));
        massValue_->setText(QStringLiteral("—"));
    } else {
        densityValue_->setText(QStringLiteral("%1 кг/м³").arg(density, 0, 'g', 5));
        if (properties.hasVolume) {
            const double massKg = properties.volumeMm3 * 1.0e-9 * density;
            massValue_->setText(massKg < 1.0
                                    ? QStringLiteral("%1 г (оценка)").arg(massKg * 1000.0, 0, 'g', 5)
                                    : QStringLiteral("%1 кг (оценка)").arg(massKg, 0, 'g', 5));
        } else {
            massValue_->setText(QStringLiteral("нет данных"));
        }
    }
}

void MainWindow::updateStatus()
{
    if (objectCountLabel_)
        objectCountLabel_->setText(QStringLiteral("%1 объект(ов)").arg(static_cast<qulonglong>(document_.objects.size())));
}

void MainWindow::updateWindowTitle()
{
    const QString documentTitle = projectPath_.isEmpty() ? document_.name : QFileInfo(projectPath_).fileName();
    const QString modifiedMarker = savedHistoryIndex_ == historyIndex_ ? QString() : QStringLiteral(" *");
    setWindowTitle(QStringLiteral("%1%2 — AoSCAD Studio %3 · C++")
                       .arg(documentTitle, modifiedMarker, QCoreApplication::applicationVersion()));
}

void MainWindow::refreshViewport()
{
    viewport_->refreshGeometry();
    viewport_->setSelectedObject(selectedIndex());
    updateTree();
    updateInspector();
    updateStatus();
}

void MainWindow::pushHistory(bool markSaved)
{
    if (historyIndex_ >= 0 && historyIndex_ < static_cast<int>(history_.size()) &&
        sameDocumentState(history_[static_cast<std::size_t>(historyIndex_)], document_)) {
        if (markSaved)
            savedHistoryIndex_ = historyIndex_;
        undoAction_->setEnabled(historyIndex_ > 0);
        redoAction_->setEnabled(historyIndex_ + 1 < static_cast<int>(history_.size()));
        updateStatus();
        updateWindowTitle();
        return;
    }
    if (historyIndex_ + 1 < static_cast<int>(history_.size())) {
        if (savedHistoryIndex_ > historyIndex_)
            savedHistoryIndex_ = -1;
        history_.erase(history_.begin() + historyIndex_ + 1, history_.end());
    }
    history_.push_back(document_);
    if (history_.size() > 50) {
        if (savedHistoryIndex_ == 0)
            savedHistoryIndex_ = -1;
        else if (savedHistoryIndex_ > 0)
            --savedHistoryIndex_;
        history_.erase(history_.begin());
    }
    historyIndex_ = static_cast<int>(history_.size()) - 1;
    if (markSaved)
        savedHistoryIndex_ = historyIndex_;
    undoAction_->setEnabled(historyIndex_ > 0);
    redoAction_->setEnabled(false);
    updateStatus();
    updateWindowTitle();
}

void MainWindow::restoreHistory(int index)
{
    if (index < 0 || index >= static_cast<int>(history_.size()) || index == historyIndex_)
        return;
    if (!pendingExtrudeSketchId_.isEmpty())
        cancelExtrude();
    historyIndex_ = index;
    document_ = history_[static_cast<std::size_t>(historyIndex_)];
    viewport_->setDocument(&document_);
    updateTree();
    updateInspector();
    undoAction_->setEnabled(historyIndex_ > 0);
    redoAction_->setEnabled(historyIndex_ + 1 < static_cast<int>(history_.size()));
    updateWindowTitle();
    statusBar()->showMessage(QStringLiteral("Состояние восстановлено"), 2500);
}

bool MainWindow::confirmSaveChanges()
{
    if (QWidget* focusedWidget = QApplication::focusWidget(); focusedWidget && isAncestorOf(focusedWidget))
        focusedWidget->clearFocus();
    if (savedHistoryIndex_ == historyIndex_)
        return true;
    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, QStringLiteral("Несохранённые изменения"),
        QStringLiteral("В проекте есть несохранённые изменения. Сохранить их перед продолжением?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return saveProject();
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (confirmSaveChanges())
        event->accept();
    else
        event->ignore();
}

int MainWindow::selectedIndex() const
{
    return viewport_ ? viewport_->selectedObject() : -1;
}

double MainWindow::unitFactor() const
{
    if (document_.units == QStringLiteral("см")) return 0.1;
    if (document_.units == QStringLiteral("дюйм")) return 1.0 / 25.4;
    return 1.0;
}

QString MainWindow::uniqueName(const QString& base) const
{
    QSet<QString> names;
    for (const CadObject& object : document_.objects)
        names.insert(object.name);
    for (int i = 1; i < 100000; ++i) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(i, 2, 10, QLatin1Char('0'));
        if (!names.contains(candidate))
            return candidate;
    }
    return base;
}

void MainWindow::updateObjectTransform(int axis, double value, int transformType)
{
    if (syncingInspector_ || axis < 0 || axis > 2)
        return;
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size())
        return;

    try {
        CadDocument candidate = document_;
        CadObject& object = candidate.objects[static_cast<std::size_t>(index)];
        if (transformType == 0)
            object.position[axis] = static_cast<float>(value / unitFactor());
        else if (transformType == 1)
            object.rotation[axis] = static_cast<float>(value);
        else if (transformType == 2)
            object.scale[axis] = static_cast<float>(std::max(0.001, value));
        else
            return;

        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);
        viewport_->refreshGeometry();
        updateTree();
        updatePhysicalInspector(document_.objects[static_cast<std::size_t>(index)]);
        if (!pendingExtrudeSketchId_.isEmpty())
            updateExtrudePreview();
    } catch (const Standard_Failure& error) {
        updateInspector();
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 5000);
    } catch (const std::exception& error) {
        updateInspector();
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::addPrimitive(const QString& kind)
{
    CadObject object;
    object.id = cadNewId();
    object.featureType = QStringLiteral("primitive");
    object.primitiveType = kind;
    if (kind == QStringLiteral("box")) {
        object.setPrimitiveParameters({40.0, 30.0, 24.0});
        object.name = uniqueName(QStringLiteral("Бокс"));
        object.color = QColor(115, 153, 170);
    } else if (kind == QStringLiteral("cylinder")) {
        object.setPrimitiveParameters({15.0, 32.0});
        object.name = uniqueName(QStringLiteral("Цилиндр"));
        object.color = QColor(91, 157, 157);
    } else if (kind == QStringLiteral("sphere")) {
        object.setPrimitiveParameters({18.0});
        object.name = uniqueName(QStringLiteral("Сфера"));
        object.color = QColor(112, 153, 183);
    } else if (kind == QStringLiteral("cone")) {
        object.setPrimitiveParameters({17.0, 0.0, 34.0});
        object.name = uniqueName(QStringLiteral("Конус"));
        object.color = QColor(164, 133, 94);
    } else if (kind == QStringLiteral("torus")) {
        object.setPrimitiveParameters({18.0, 4.0});
        object.name = uniqueName(QStringLiteral("Тор"));
        object.color = QColor(130, 157, 147);
    } else {
        return;
    }

    object.position.setX(static_cast<float>(document_.objects.size()) * 55.0F);
    document_.objects.push_back(std::move(object));
    const int index = static_cast<int>(document_.objects.size()) - 1;
    refreshViewport();
    viewport_->setSelectedObject(index);
    updateTree();
    pushHistory();
    viewport_->fitAll();
    statusBar()->showMessage(QStringLiteral("Добавлен объект: %1").arg(document_.objects.back().name), 2500);
}

void MainWindow::editPrimitiveParameters()
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size())
        return;
    const CadObject& selected = document_.objects[static_cast<std::size_t>(index)];
    const bool hasEditableParameters = selected.featureType == QStringLiteral("primitive") ||
        selected.featureType == QStringLiteral("sketch") ||
        selected.featureType == QStringLiteral("fillet") ||
        selected.featureType == QStringLiteral("chamfer") ||
        selected.featureType == QStringLiteral("pattern");
    if (!hasEditableParameters)
        return;
    const int parameterCount = primitiveParameterCount(selected.primitiveType);
    if (parameterCount <= 0 || parameterCount != static_cast<int>(selected.primitiveParameters.size()))
        return;

    try {
        CadDocument candidate = document_;
        CadObject& object = candidate.objects[static_cast<std::size_t>(index)];
        const ExpressionParser::Values variableValues = ExpressionParser::evaluateVariables(candidate.variables);
        object.parameterExpressions.clear();
        object.primitiveParameters.clear();
        for (int i = 0; i < parameterCount; ++i) {
            const QString expression = primitiveFields_[static_cast<std::size_t>(i)]->text().trimmed();
            if (expression.isEmpty())
                throw ExpressionError(QStringLiteral("Параметр не может быть пустым."));
            const double value = ExpressionParser::evaluate(expression, variableValues);
            object.parameterExpressions.insert(QStringLiteral("p%1").arg(i), expression);
            object.primitiveParameters.push_back(value);
        }
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);
        for (QLineEdit* field : primitiveFields_)
            field->setStyleSheet(QString());
        refreshViewport();
        if (!pendingExtrudeSketchId_.isEmpty())
            updateExtrudePreview();
        pushHistory();
        statusBar()->showMessage(QStringLiteral("Формулы параметров пересчитаны"), 2500);
    } catch (const Standard_Failure& error) {
        for (QLineEdit* field : primitiveFields_)
            field->setStyleSheet(QStringLiteral("border:1px solid #d86464;"));
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 5000);
    } catch (const std::exception& error) {
        for (QLineEdit* field : primitiveFields_)
            field->setStyleSheet(QStringLiteral("border:1px solid #d86464;"));
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::createBooleanFeature(const QString& operation)
{
    QString title;
    QString resultName;
    if (operation == QStringLiteral("union")) {
        title = QStringLiteral("Объединение тел");
        resultName = QStringLiteral("Объединение");
    } else if (operation == QStringLiteral("cut")) {
        title = QStringLiteral("Вычитание тел");
        resultName = QStringLiteral("Вычитание");
    } else if (operation == QStringLiteral("common")) {
        title = QStringLiteral("Пересечение тел");
        resultName = QStringLiteral("Пересечение");
    } else {
        statusBar()->showMessage(QStringLiteral("Неизвестная булева операция."), 4000);
        return;
    }

    std::vector<int> eligibleObjects;
    try {
        for (std::size_t i = 0; i < document_.objects.size(); ++i) {
            const CadObject& object = document_.objects[i];
            if (!object.visible || object.suppressed || object.featureType == QStringLiteral("sketch"))
                continue;
            if (CadIO::analyzeShape(object.worldShape()).hasVolume)
                eligibleObjects.push_back(static_cast<int>(i));
        }
    } catch (const Standard_Failure& error) {
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 5000);
        return;
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
        return;
    }

    if (eligibleObjects.size() < 2) {
        statusBar()->showMessage(QStringLiteral("Для булевой операции нужны два видимых тела с объёмом."), 4500);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* baseCombo = new QComboBox(&dialog);
    auto* toolCombo = new QComboBox(&dialog);
    for (const int index : eligibleObjects) {
        const CadObject& object = document_.objects[static_cast<std::size_t>(index)];
        const QString label = QStringLiteral("%1  [%2]").arg(object.name).arg(index + 1);
        baseCombo->addItem(label, object.id);
        toolCombo->addItem(label, object.id);
    }
    form->addRow(QStringLiteral("Основание"), baseCombo);
    form->addRow(QStringLiteral("Инструмент"), toolCombo);
    layout->addLayout(form);
    auto* hint = new QLabel(QStringLiteral("Для вычитания используется порядок: основание минус инструмент. Исходные тела останутся в дереве, но будут скрыты."), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    const int selected = selectedIndex();
    for (int i = 0; i < baseCombo->count(); ++i) {
        const int documentIndex = eligibleObjects[static_cast<std::size_t>(i)];
        if (documentIndex == selected) {
            baseCombo->setCurrentIndex(i);
            break;
        }
    }
    int firstDifferent = 0;
    for (int i = 0; i < toolCombo->count(); ++i) {
        if (toolCombo->itemData(i).toString() != baseCombo->currentData().toString()) {
            firstDifferent = i;
            break;
        }
    }
    toolCombo->setCurrentIndex(firstDifferent);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString baseId = baseCombo->currentData().toString();
    const QString toolId = toolCombo->currentData().toString();
    if (baseId.isEmpty() || toolId.isEmpty() || baseId == toolId) {
        QMessageBox::warning(this, title, QStringLiteral("Основание и инструмент должны быть разными телами."));
        return;
    }

    try {
        CadDocument candidate = document_;
        const auto baseIt = std::find_if(candidate.objects.begin(), candidate.objects.end(), [&baseId](const CadObject& object) {
            return object.id == baseId;
        });
        const auto toolIt = std::find_if(candidate.objects.begin(), candidate.objects.end(), [&toolId](const CadObject& object) {
            return object.id == toolId;
        });
        if (baseIt == candidate.objects.end() || toolIt == candidate.objects.end())
            throw CadError(QStringLiteral("Одно из выбранных тел больше не существует."));

        CadObject feature;
        feature.id = cadNewId();
        feature.name = uniqueName(resultName);
        feature.featureType = QStringLiteral("boolean");
        feature.primitiveType = operation;
        feature.inputs = {baseId, toolId};
        feature.color = baseIt->color;
        feature.materialId = baseIt->materialId;
        baseIt->visible = false;
        toolIt->visible = false;
        candidate.objects.push_back(std::move(feature));
        const QString resultId = candidate.objects.back().id;
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);

        int resultIndex = -1;
        for (std::size_t i = 0; i < document_.objects.size(); ++i) {
            if (document_.objects[i].id == resultId) {
                resultIndex = static_cast<int>(i);
                break;
            }
        }
        viewport_->setSelectedObject(resultIndex);
        refreshViewport();
        pushHistory();
        viewport_->fitAll();
        statusBar()->showMessage(QStringLiteral("%1 построено и добавлено в дерево фич.").arg(title), 4000);
    } catch (const Standard_Failure& error) {
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 6000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
    }
}

void MainWindow::createEdgeTreatmentFeature(bool chamfer)
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size()) {
        statusBar()->showMessage(QStringLiteral("Сначала выберите тело для редактирования."), 4000);
        return;
    }

    const CadObject& selected = document_.objects[static_cast<std::size_t>(index)];
    const QString operationName = chamfer ? QStringLiteral("Фаска") : QStringLiteral("Скругление");
    try {
        const CadGeometryProperties properties = CadIO::analyzeShape(selected.worldShape());
        if (!properties.hasVolume) {
            statusBar()->showMessage(QStringLiteral("%1 требует замкнутое тело с объёмом; эскизы и открытые поверхности не подходят.").arg(operationName), 5000);
            return;
        }
        const float minExtent = std::min({properties.boundsMm.x(), properties.boundsMm.y(), properties.boundsMm.z()});
        const double suggestedSize = std::max(0.001, std::min(1.0, static_cast<double>(minExtent) * 0.05));
        bool accepted = false;
        const double size = QInputDialog::getDouble(
            this,
            chamfer ? QStringLiteral("Фаска всех рёбер") : QStringLiteral("Скругление всех рёбер"),
            chamfer ? QStringLiteral("Размер фаски для всех рёбер · мм")
                    : QStringLiteral("Радиус скругления для всех рёбер · мм"),
            suggestedSize, 0.001, 1000000.0, 3, &accepted);
        if (!accepted)
            return;

        CadDocument candidate = document_;
        CadObject& source = candidate.objects[static_cast<std::size_t>(index)];
        CadObject feature;
        feature.id = cadNewId();
        feature.name = uniqueName(chamfer ? QStringLiteral("Фаска") : QStringLiteral("Скругление"));
        feature.featureType = chamfer ? QStringLiteral("chamfer") : QStringLiteral("fillet");
        feature.primitiveType = feature.featureType;
        feature.inputs = {source.id};
        feature.primitiveParameters = {size};
        feature.parameterExpressions.insert(QStringLiteral("p0"), QString::number(size, 'g', 15));
        feature.color = source.color;
        feature.materialId = source.materialId;
        source.visible = false;
        candidate.objects.push_back(std::move(feature));
        const QString resultId = candidate.objects.back().id;
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);

        int resultIndex = -1;
        for (std::size_t i = 0; i < document_.objects.size(); ++i) {
            if (document_.objects[i].id == resultId) {
                resultIndex = static_cast<int>(i);
                break;
            }
        }
        viewport_->setSelectedObject(resultIndex);
        refreshViewport();
        pushHistory();
        statusBar()->showMessage(QStringLiteral("%1 всех рёбер выполнено. Размер можно изменить в инспекторе.").arg(operationName), 5000);
    } catch (const Standard_Failure& error) {
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 6000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
    }
}

void MainWindow::createLinearPatternFeature(const QString& axis)
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size()) {
        statusBar()->showMessage(QStringLiteral("Сначала выберите тело для массива."), 4000);
        return;
    }

    const QString normalizedAxis = axis.trimmed().toUpper();
    if (normalizedAxis != QStringLiteral("X") && normalizedAxis != QStringLiteral("Y") && normalizedAxis != QStringLiteral("Z")) {
        statusBar()->showMessage(QStringLiteral("Ось массива должна быть X, Y или Z."), 4000);
        return;
    }

    try {
        const CadObject& selected = document_.objects[static_cast<std::size_t>(index)];
        const CadGeometryProperties properties = CadIO::analyzeShape(selected.worldShape());
        if (!properties.hasVolume) {
            statusBar()->showMessage(QStringLiteral("Линейный массив доступен только для тела с объёмом."), 5000);
            return;
        }
        bool accepted = false;
        const int instanceCount = QInputDialog::getInt(
            this, QStringLiteral("Линейный массив"), QStringLiteral("Количество экземпляров (включая исходное тело)"),
            3, 2, 100, 1, &accepted);
        if (!accepted)
            return;

        const int axisIndex = normalizedAxis == QStringLiteral("X") ? 0 : normalizedAxis == QStringLiteral("Y") ? 1 : 2;
        const double extent = std::max(0.001, static_cast<double>(properties.boundsMm[axisIndex]));
        const double suggestedSpacing = std::min(1000000.0, std::max(0.001, extent * 1.2));
        const double spacing = QInputDialog::getDouble(
            this, QStringLiteral("Линейный массив"), QStringLiteral("Шаг вдоль оси %1 · мм").arg(normalizedAxis),
            suggestedSpacing, 0.001, 1000000.0, 3, &accepted);
        if (!accepted)
            return;

        CadDocument candidate = document_;
        CadObject& source = candidate.objects[static_cast<std::size_t>(index)];
        CadObject feature;
        feature.id = cadNewId();
        feature.name = uniqueName(QStringLiteral("Линейный массив %1").arg(normalizedAxis));
        feature.featureType = QStringLiteral("pattern");
        feature.primitiveType = QStringLiteral("linear-pattern-%1").arg(normalizedAxis.toLower());
        feature.inputs = {source.id};
        feature.primitiveParameters = {static_cast<double>(instanceCount), spacing};
        feature.parameterExpressions.insert(QStringLiteral("p0"), QString::number(instanceCount));
        feature.parameterExpressions.insert(QStringLiteral("p1"), QString::number(spacing, 'g', 15));
        feature.color = source.color;
        feature.materialId = source.materialId;
        source.visible = false;
        candidate.objects.push_back(std::move(feature));
        const QString resultId = candidate.objects.back().id;
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);

        int resultIndex = -1;
        for (std::size_t i = 0; i < document_.objects.size(); ++i) {
            if (document_.objects[i].id == resultId) {
                resultIndex = static_cast<int>(i);
                break;
            }
        }
        viewport_->setSelectedObject(resultIndex);
        refreshViewport();
        pushHistory();
        viewport_->fitAll();
        statusBar()->showMessage(QStringLiteral("Линейный массив по оси %1 создан: %2 экземпляра(ов).")
                                     .arg(normalizedAxis).arg(instanceCount), 5000);
    } catch (const Standard_Failure& error) {
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 6000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
    }
}

void MainWindow::createMirrorFeature(const QString& plane)
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size()) {
        statusBar()->showMessage(QStringLiteral("Сначала выберите тело для зеркального отражения."), 4000);
        return;
    }

    const QString normalizedPlane = plane.trimmed().toUpper();
    if (normalizedPlane != QStringLiteral("XY") && normalizedPlane != QStringLiteral("YZ") &&
        normalizedPlane != QStringLiteral("ZX")) {
        statusBar()->showMessage(QStringLiteral("Плоскость зеркального отражения должна быть XY, YZ или ZX."), 4000);
        return;
    }

    try {
        CadDocument candidate = document_;
        CadObject& source = candidate.objects[static_cast<std::size_t>(index)];
        if (!CadIO::analyzeShape(source.worldShape()).hasVolume) {
            statusBar()->showMessage(QStringLiteral("Зеркальное отражение доступно только для тела с объёмом."), 5000);
            return;
        }
        CadObject feature;
        feature.id = cadNewId();
        feature.name = uniqueName(QStringLiteral("Зеркальная копия %1").arg(normalizedPlane));
        feature.featureType = QStringLiteral("mirror");
        feature.primitiveType = QStringLiteral("mirror-%1").arg(normalizedPlane.toLower());
        feature.inputs = {source.id};
        feature.color = source.color;
        feature.materialId = source.materialId;
        source.visible = false;
        candidate.objects.push_back(std::move(feature));
        const QString resultId = candidate.objects.back().id;
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);

        int resultIndex = -1;
        for (std::size_t i = 0; i < document_.objects.size(); ++i) {
            if (document_.objects[i].id == resultId) {
                resultIndex = static_cast<int>(i);
                break;
            }
        }
        viewport_->setSelectedObject(resultIndex);
        refreshViewport();
        pushHistory();
        viewport_->fitAll();
        statusBar()->showMessage(QStringLiteral("Зеркальная копия относительно %1 создана.").arg(normalizedPlane), 4500);
    } catch (const Standard_Failure& error) {
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 6000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
    }
}

void MainWindow::startSketch(CadViewport::SketchTool tool)
{
    if (!pendingExtrudeSketchId_.isEmpty())
        cancelExtrude();
    const CadViewport::ReferencePlane plane = viewport_->activeReferencePlane();
    if (plane == CadViewport::ReferencePlane::None) {
        statusBar()->showMessage(QStringLiteral("Сначала выберите XY, YZ или ZX в дереве проекта."), 4500);
        return;
    }
    viewport_->beginSketch(tool, plane);
    statusBar()->showMessage(QStringLiteral("Эскиз: нажмите и перетащите мышь; координаты привязываются к сетке 5 мм. Esc — отмена."), 6000);
}

void MainWindow::createSketch(const QString& profileType, const QVector2D& origin, const QVector2D& size)
{
    const CadViewport::ReferencePlane activePlane = viewport_->activeReferencePlane();
    if (activePlane == CadViewport::ReferencePlane::None) {
        statusBar()->showMessage(QStringLiteral("Создание эскиза отменено: базовая плоскость не выбрана."), 4000);
        return;
    }

    CadObject sketch;
    sketch.id = cadNewId();
    sketch.name = uniqueName(QStringLiteral("Эскиз"));
    sketch.featureType = QStringLiteral("sketch");
    sketch.primitiveType = profileType;
    sketch.sketchPlane = activePlane == CadViewport::ReferencePlane::XY ? QStringLiteral("XY")
        : activePlane == CadViewport::ReferencePlane::YZ ? QStringLiteral("YZ")
        : QStringLiteral("ZX");
    sketch.sketchOrigin = origin;
    sketch.color = QColor(105, 206, 184);
    if (profileType == QStringLiteral("sketch-rectangle")) {
        sketch.sketchConstraints = {QStringLiteral("horizontal"), QStringLiteral("vertical"),
                                    QStringLiteral("coincident-closed"), QStringLiteral("grid-snap-5mm")};
        sketch.primitiveParameters = {size.x(), size.y()};
        sketch.parameterExpressions.insert(QStringLiteral("p0"), QString::number(size.x(), 'g', 12));
        sketch.parameterExpressions.insert(QStringLiteral("p1"), QString::number(size.y(), 'g', 12));
    } else if (profileType == QStringLiteral("sketch-circle")) {
        sketch.sketchConstraints = {QStringLiteral("center-fixed"), QStringLiteral("radius-dimension"),
                                    QStringLiteral("grid-snap-5mm")};
        sketch.primitiveParameters = {size.x()};
        sketch.parameterExpressions.insert(QStringLiteral("p0"), QString::number(size.x(), 'g', 12));
    } else {
        statusBar()->showMessage(QStringLiteral("Этот профиль эскиза не поддерживается."), 4000);
        return;
    }

    try {
        CadDocument candidate = document_;
        candidate.objects.push_back(std::move(sketch));
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);
        const int index = static_cast<int>(document_.objects.size()) - 1;
        viewport_->refreshGeometry();
        viewport_->setSelectedObject(index);
        updateTree();
        updateInspector();
        pushHistory();
        viewport_->fitAll();
        statusBar()->showMessage(QStringLiteral("Эскиз создан. Размеры редактируются формулами в панели свойств."), 4500);
    } catch (const Standard_Failure& error) {
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 5000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::beginExtrudeForSelectedSketch()
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size() ||
        document_.objects[static_cast<std::size_t>(index)].featureType != QStringLiteral("sketch")) {
        statusBar()->showMessage(QStringLiteral("Выберите замкнутый эскиз в дереве или viewport."), 4500);
        return;
    }
    beginExtrude(document_.objects[static_cast<std::size_t>(index)].id);
}

void MainWindow::beginExtrudeEditForSelectedFeature()
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size())
        return;
    const CadObject& feature = document_.objects[static_cast<std::size_t>(index)];
    if (feature.featureType != QStringLiteral("extrude") || feature.inputs.size() != 1) {
        statusBar()->showMessage(QStringLiteral("Выберите фичу выдавливания для редактирования."), 3500);
        return;
    }
    beginExtrude(feature.inputs.front(), feature.id);
}

void MainWindow::beginExtrude(const QString& sketchId, const QString& targetId)
{
    if (!pendingExtrudeSketchId_.isEmpty())
        cancelExtrude();
    const auto sketchIt = std::find_if(document_.objects.cbegin(), document_.objects.cend(), [&sketchId](const CadObject& object) {
        return object.id == sketchId && object.featureType == QStringLiteral("sketch");
    });
    if (sketchIt == document_.objects.cend()) {
        statusBar()->showMessage(QStringLiteral("Исходный эскиз не найден."), 4500);
        return;
    }

    pendingExtrudeSketchId_ = sketchId;
    pendingExtrudeTargetId_ = targetId;
    QString initialExpression = QStringLiteral("20");
    bool symmetric = false;
    if (!targetId.isEmpty()) {
        const auto targetIt = std::find_if(document_.objects.cbegin(), document_.objects.cend(), [&targetId](const CadObject& object) {
            return object.id == targetId;
        });
        if (targetIt != document_.objects.cend()) {
            initialExpression = targetIt->parameterExpressions.value(QStringLiteral("p0"),
                targetIt->primitiveParameters.empty() ? QStringLiteral("20")
                    : QString::number(targetIt->primitiveParameters.front(), 'g', 12));
            symmetric = targetIt->symmetricExtrude;
        }
    }
    viewport_->setTemporarilyHiddenObject(-1);
    {
        const QSignalBlocker lengthBlock(extrudeLengthField_);
        const QSignalBlocker symmetricBlock(symmetricExtrudeCheck_);
        extrudeLengthField_->setText(initialExpression);
        symmetricExtrudeCheck_->setChecked(symmetric);
    }
    extrudeGroup_->setVisible(true);
    editExtrudeButton_->setVisible(false);
    updateExtrudePreview();
    extrudeLengthField_->setFocus();
    extrudeLengthField_->selectAll();
}

void MainWindow::updateExtrudePreview()
{
    if (pendingExtrudeSketchId_.isEmpty() || !extrudeLengthField_)
        return;
    hasValidExtrudePreview_ = false;
    extrudeCommitButton_->setEnabled(false);
    try {
        const auto sketchIt = std::find_if(document_.objects.cbegin(), document_.objects.cend(), [this](const CadObject& object) {
            return object.id == pendingExtrudeSketchId_ && object.featureType == QStringLiteral("sketch");
        });
        if (sketchIt == document_.objects.cend())
            throw CadError(QStringLiteral("Исходный эскиз не найден."));
        const ExpressionParser::Values variableValues = ExpressionParser::evaluateVariables(document_.variables);
        const QString expression = extrudeLengthField_->text().trimmed();
        const double length = ExpressionParser::evaluate(expression, variableValues);
        TopoDS_Shape preview = CadIO::makeExtrusion(*sketchIt, length, symmetricExtrudeCheck_->isChecked());
        int hiddenTargetIndex = -1;
        if (!pendingExtrudeTargetId_.isEmpty()) {
            for (std::size_t i = 0; i < document_.objects.size(); ++i) {
                if (document_.objects[i].id != pendingExtrudeTargetId_)
                    continue;
                hiddenTargetIndex = static_cast<int>(i);
                CadObject transformedPreview = document_.objects[i];
                transformedPreview.shape = preview;
                preview = transformedPreview.worldShape();
                break;
            }
        }
        viewport_->setTemporarilyHiddenObject(hiddenTargetIndex);
        viewport_->setPreviewShape(preview);
        hasValidExtrudePreview_ = true;
        extrudeCommitButton_->setEnabled(true);
        setExtrudeStatusLevel(QStringLiteral("success"));
        extrudeStatusLabel_->setText(QStringLiteral("Предпросмотр обновлён · %1 мм. Подтвердите фичу или отмените.").arg(length, 0, 'g', 8));
    } catch (const Standard_Failure& error) {
        viewport_->setTemporarilyHiddenObject(-1);
        viewport_->clearPreviewShape();
        setExtrudeStatusLevel(QStringLiteral("error"));
        extrudeStatusLabel_->setText(QString::fromLocal8Bit(error.GetMessageString()));
    } catch (const std::exception& error) {
        viewport_->setTemporarilyHiddenObject(-1);
        viewport_->clearPreviewShape();
        setExtrudeStatusLevel(QStringLiteral("error"));
        extrudeStatusLabel_->setText(QString::fromUtf8(error.what()));
    }
}

void MainWindow::confirmExtrude()
{
    if (!hasValidExtrudePreview_ || pendingExtrudeSketchId_.isEmpty())
        return;
    try {
        CadDocument candidate = document_;
        const auto sketchIt = std::find_if(candidate.objects.cbegin(), candidate.objects.cend(), [this](const CadObject& object) {
            return object.id == pendingExtrudeSketchId_ && object.featureType == QStringLiteral("sketch");
        });
        if (sketchIt == candidate.objects.cend())
            throw CadError(QStringLiteral("Исходный эскиз не найден."));
        const QString expression = extrudeLengthField_->text().trimmed();
        const ExpressionParser::Values variableValues = ExpressionParser::evaluateVariables(candidate.variables);
        const double length = ExpressionParser::evaluate(expression, variableValues);

        int targetIndex = -1;
        if (!pendingExtrudeTargetId_.isEmpty()) {
            for (std::size_t i = 0; i < candidate.objects.size(); ++i) {
                if (candidate.objects[i].id == pendingExtrudeTargetId_) {
                    targetIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        if (targetIndex < 0) {
            CadObject feature;
            feature.id = cadNewId();
            feature.name = uniqueName(QStringLiteral("Выдавливание"));
            feature.featureType = QStringLiteral("extrude");
            feature.primitiveType = QStringLiteral("extrude");
            feature.inputs = {pendingExtrudeSketchId_};
            feature.primitiveParameters = {length};
            feature.parameterExpressions.insert(QStringLiteral("p0"), expression);
            feature.sketchPlane = sketchIt->sketchPlane;
            feature.symmetricExtrude = symmetricExtrudeCheck_->isChecked();
            feature.color = QColor(103, 164, 166);
            candidate.objects.push_back(std::move(feature));
            targetIndex = static_cast<int>(candidate.objects.size()) - 1;
        } else {
            CadObject& feature = candidate.objects[static_cast<std::size_t>(targetIndex)];
            feature.featureType = QStringLiteral("extrude");
            feature.primitiveType = QStringLiteral("extrude");
            feature.inputs = {pendingExtrudeSketchId_};
            feature.primitiveParameters = {length};
            feature.parameterExpressions.insert(QStringLiteral("p0"), expression);
            feature.sketchPlane = sketchIt->sketchPlane;
            feature.symmetricExtrude = symmetricExtrudeCheck_->isChecked();
        }

        const QString committedId = candidate.objects[static_cast<std::size_t>(targetIndex)].id;
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);
        pendingExtrudeSketchId_.clear();
        pendingExtrudeTargetId_.clear();
        hasValidExtrudePreview_ = false;
        extrudeGroup_->setVisible(false);
        viewport_->setTemporarilyHiddenObject(-1);
        viewport_->clearPreviewShape();
        viewport_->refreshGeometry();
        int selected = -1;
        for (std::size_t i = 0; i < document_.objects.size(); ++i) {
            if (document_.objects[i].id == committedId) {
                selected = static_cast<int>(i);
                break;
            }
        }
        viewport_->setSelectedObject(selected);
        updateTree();
        updateInspector();
        pushHistory();
        viewport_->fitAll();
        statusBar()->showMessage(QStringLiteral("Выдавливание добавлено в дерево построения."), 4000);
    } catch (const Standard_Failure& error) {
        statusBar()->showMessage(QString::fromLocal8Bit(error.GetMessageString()), 5000);
    } catch (const std::exception& error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()), 5000);
    }
}

void MainWindow::cancelExtrude()
{
    pendingExtrudeSketchId_.clear();
    pendingExtrudeTargetId_.clear();
    hasValidExtrudePreview_ = false;
    extrudeGroup_->setVisible(false);
    viewport_->setTemporarilyHiddenObject(-1);
    viewport_->clearPreviewShape();
    updateInspector();
    statusBar()->showMessage(QStringLiteral("Предпросмотр отменён; документ не изменён."), 3000);
}

void MainWindow::applyVariablesFromTable()
{
    if (populatingVariables_ || !variablesTable_)
        return;
    ExpressionParser::Definitions definitions;
    const QRegularExpression identifier(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    const QSet<QString> reserved{
        QStringLiteral("pi"), QStringLiteral("sin"), QStringLiteral("cos"), QStringLiteral("sqrt"),
        QStringLiteral("min"), QStringLiteral("max")
    };
    for (int row = 0; row < variablesTable_->rowCount(); ++row) {
        const QString name = variablesTable_->item(row, 0) ? variablesTable_->item(row, 0)->text().trimmed() : QString();
        const QString expression = variablesTable_->item(row, 1) ? variablesTable_->item(row, 1)->text().trimmed() : QString();
        if (!identifier.match(name).hasMatch() || reserved.contains(name.toLower())) {
            variablesErrorLabel_->setText(QStringLiteral("Имя переменной «%1» некорректно или зарезервировано.").arg(name));
            return;
        }
        if (expression.isEmpty()) {
            variablesErrorLabel_->setText(QStringLiteral("Формула переменной «%1» не может быть пустой.").arg(name));
            return;
        }
        if (definitions.contains(name)) {
            variablesErrorLabel_->setText(QStringLiteral("Переменная «%1» указана больше одного раза.").arg(name));
            return;
        }
        definitions.insert(name, expression);
    }

    try {
        (void)ExpressionParser::evaluateVariables(definitions);
        CadDocument candidate = document_;
        candidate.variables = definitions;
        CadIO::recomputeParametricFeatures(candidate);
        document_ = std::move(candidate);
        variablesErrorLabel_->clear();
        refreshViewport();
        if (!pendingExtrudeSketchId_.isEmpty())
            updateExtrudePreview();
        pushHistory();
    } catch (const Standard_Failure& error) {
        variablesErrorLabel_->setText(QString::fromLocal8Bit(error.GetMessageString()));
        statusBar()->showMessage(variablesErrorLabel_->text(), 5000);
    } catch (const std::exception& error) {
        variablesErrorLabel_->setText(QString::fromUtf8(error.what()));
        statusBar()->showMessage(variablesErrorLabel_->text(), 5000);
    }
}

void MainWindow::updateVariablesTable()
{
    if (!variablesTable_)
        return;
    populatingVariables_ = true;
    const QSignalBlocker blocker(variablesTable_);
    variablesTable_->setRowCount(document_.variables.size());
    int row = 0;
    for (auto it = document_.variables.cbegin(); it != document_.variables.cend(); ++it, ++row) {
        variablesTable_->setItem(row, 0, new QTableWidgetItem(it.key()));
        variablesTable_->setItem(row, 1, new QTableWidgetItem(it.value()));
    }
    populatingVariables_ = false;
}

void MainWindow::importFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Импорт CAD-файлов"), {}, openFilter());
    if (paths.isEmpty())
        return;
    bool imported = false;
    int lastImportedIndex = -1;
    for (const QString& path : paths) {
        try {
            CadObject object = CadIO::importFile(path);
            object.name = uniqueName(object.name);
            document_.objects.push_back(std::move(object));
            lastImportedIndex = static_cast<int>(document_.objects.size()) - 1;
            imported = true;
        } catch (const CadError& error) {
            QMessageBox::warning(this, QStringLiteral("Ошибка импорта"), QString::fromUtf8(error.what()));
        } catch (const Standard_Failure& error) {
            QMessageBox::warning(this, QStringLiteral("Ошибка импорта"), QString::fromLocal8Bit(error.GetMessageString()));
        } catch (const std::exception& error) {
            QMessageBox::warning(this, QStringLiteral("Ошибка импорта"), QString::fromUtf8(error.what()));
        }
    }
    if (imported) {
        refreshViewport();
        viewport_->setSelectedObject(lastImportedIndex);
        updateTree();
        updateInspector();
        pushHistory();
        viewport_->fitAll();
        statusBar()->showMessage(QStringLiteral("Импорт завершён"), 3000);
    }
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Открыть проект AoSCAD"), {},
                                                      QStringLiteral("Проект AoSCAD (*.aoscad);;Все файлы (*.*)"));
    if (path.isEmpty() || !confirmSaveChanges())
        return;
    if (!pendingExtrudeSketchId_.isEmpty())
        cancelExtrude();
    try {
        document_ = CadIO::loadProject(path);
        projectPath_ = QFileInfo(path).absoluteFilePath();
        history_.clear();
        historyIndex_ = -1;
        savedHistoryIndex_ = -1;
        viewport_->setDocument(&document_);
        updateTree();
        updateInspector();
        pushHistory(true);
        viewport_->fitAll();
        statusBar()->showMessage(QStringLiteral("Проект открыт: %1").arg(QFileInfo(path).fileName()), 3500);
    } catch (const CadError& error) {
        QMessageBox::critical(this, QStringLiteral("Не удалось открыть проект"), QString::fromUtf8(error.what()));
    } catch (const Standard_Failure& error) {
        QMessageBox::critical(this, QStringLiteral("Не удалось открыть проект"), QString::fromLocal8Bit(error.GetMessageString()));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Не удалось открыть проект"), QString::fromUtf8(error.what()));
    }
}

bool MainWindow::saveProject(bool saveAs)
{
    if (QWidget* focusedWidget = QApplication::focusWidget(); focusedWidget && isAncestorOf(focusedWidget))
        focusedWidget->clearFocus();
    QString path = projectPath_;
    if (saveAs || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить проект AoSCAD"),
                                            document_.name + QStringLiteral(".aoscad"), saveFilter(QStringLiteral("aoscad")));
    }
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".aoscad");
    try {
        document_.name = projectName_->text().trimmed().isEmpty() ? document_.name : projectName_->text().trimmed();
        CadIO::saveProject(document_, path);
        projectPath_ = QFileInfo(path).absoluteFilePath();
        savedHistoryIndex_ = historyIndex_;
        updateInspector();
        updateWindowTitle();
        statusBar()->showMessage(QStringLiteral("Проект сохранён: %1").arg(QFileInfo(path).fileName()), 4000);
        return true;
    } catch (const CadError& error) {
        QMessageBox::critical(this, QStringLiteral("Ошибка сохранения"), QString::fromUtf8(error.what()));
    } catch (const Standard_Failure& error) {
        QMessageBox::critical(this, QStringLiteral("Ошибка сохранения"), QString::fromLocal8Bit(error.GetMessageString()));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Ошибка сохранения"), QString::fromUtf8(error.what()));
    }
    return false;
}

void MainWindow::exportTo(const QString& extension)
{
    if (document_.objects.empty()) {
        QMessageBox::information(this, QStringLiteral("Экспорт"), QStringLiteral("Сначала добавьте или импортируйте геометрию."));
        return;
    }
    const QString ext = extension == QStringLiteral("step") ? QStringLiteral("step") : extension;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Экспорт модели"),
                                                 document_.name + QLatin1Char('.') + ext, saveFilter(ext));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QLatin1Char('.') + ext;
    try {
        CadIO::exportFile(document_, path);
        const QString format = QFileInfo(path).suffix().toUpper();
        statusBar()->showMessage(QStringLiteral("Экспортировано: %1").arg(QFileInfo(path).fileName()), 5000);
        if (format == QStringLiteral("STL"))
            statusBar()->showMessage(QStringLiteral("STL экспортирован — его можно импортировать в Aeros Engine."), 6000);
    } catch (const CadError& error) {
        QMessageBox::critical(this, QStringLiteral("Ошибка экспорта"), QString::fromUtf8(error.what()));
    } catch (const Standard_Failure& error) {
        QMessageBox::critical(this, QStringLiteral("Ошибка экспорта"), QString::fromLocal8Bit(error.GetMessageString()));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, QStringLiteral("Ошибка экспорта"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::newProject()
{
    if (!confirmSaveChanges())
        return;
    if (!pendingExtrudeSketchId_.isEmpty())
        cancelExtrude();
    document_ = CadDocument{};
    projectPath_.clear();
    history_.clear();
    historyIndex_ = -1;
    savedHistoryIndex_ = -1;
    viewport_->setDocument(&document_);
    updateTree();
    updateInspector();
    pushHistory(true);
}

void MainWindow::deleteSelected()
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size())
        return;
    if (!pendingExtrudeSketchId_.isEmpty())
        cancelExtrude();
    const QString id = document_.objects[static_cast<std::size_t>(index)].id;
    for (const CadObject& candidate : document_.objects) {
        if (candidate.id != id && candidate.inputs.contains(id)) {
            statusBar()->showMessage(QStringLiteral("Нельзя удалить фичу, пока на неё ссылаются: %1").arg(candidate.name), 5000);
            return;
        }
    }
    document_.objects.erase(document_.objects.begin() + index);
    refreshViewport();
    pushHistory();
}

void MainWindow::duplicateSelected()
{
    const int index = selectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= document_.objects.size())
        return;
    CadObject copy = document_.objects[static_cast<std::size_t>(index)];
    copy.id = cadNewId();
    copy.name = uniqueName(copy.name + QStringLiteral(" копия"));
    copy.position.setX(copy.position.x() + 25.0F);
    document_.objects.push_back(std::move(copy));
    const int newIndex = static_cast<int>(document_.objects.size()) - 1;
    refreshViewport();
    viewport_->setSelectedObject(newIndex);
    updateTree();
    pushHistory();
}

void MainWindow::setTool(CadViewport::Tool tool)
{
    viewport_->setTool(tool);
    const QString names[]{QStringLiteral("Выбор"), QStringLiteral("Перемещение"), QStringLiteral("Вращение"),
                          QStringLiteral("Масштаб"), QStringLiteral("Измерение")};
    statusBar()->showMessage(QStringLiteral("Инструмент: %1").arg(names[static_cast<int>(tool)]), 1800);
}

void MainWindow::launchUpdater()
{
    const QString updaterPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("aoscad-updater.exe"));
    if (!QFileInfo::exists(updaterPath)) {
        QMessageBox::information(this, QStringLiteral("Обновление AoSCAD Studio"),
                                 QStringLiteral("Автономный updater не найден. Установите приложение из полного Windows-пакета."));
        return;
    }
    const QStringList arguments{
        QStringLiteral("--parent-pid"),
        QString::number(QCoreApplication::applicationPid())
    };
    if (!QProcess::startDetached(updaterPath, arguments, QCoreApplication::applicationDirPath())) {
        QMessageBox::warning(this, QStringLiteral("Обновление AoSCAD Studio"),
                             QStringLiteral("Не удалось запустить помощник обновления."));
    }
}

void MainWindow::showCompatibilityHelp()
{
    QMessageBox::information(this, QStringLiteral("Форматы и совместимость CAD"),
        QStringLiteral("AoSCAD Studio — отдельная CAD-среда на C++ и Open CASCADE.\n\n"
                       "Импорт: STEP / STP, IGES / IGS, BREP, STL и OBJ. Экспорт: STEP AP214/AP242, IGES, BREP, STL и OBJ.\n\n"
                       "Нативные файлы SolidWorks (.SLDPRT / .SLDASM) и T-FLEX (.TFC / .GRB) напрямую не открываются. "
                       "Откройте модель в исходной CAD-системе и экспортируйте в STEP AP214/AP242 или IGES.\n\n"
                       "Экспортируйте STL для Aeros Engine. При обмене через STL/OBJ передаётся полигональная сетка, а не параметрическая история CAD."));
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("AoSCAD Studio"),
        QStringLiteral("<h3>AoSCAD Studio %1</h3><p>Независимая CAD-среда для просмотра, подготовки и обмена 3D-геометрией.</p>"
                       "<p>Нативное приложение на C++ / Qt 6 / Open CASCADE.</p>")
            .arg(QCoreApplication::applicationVersion()));
}
