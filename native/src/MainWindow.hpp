#pragma once

#include "CadModel.hpp"
#include "CadViewport.hpp"

#include <QMainWindow>

#include <array>
#include <vector>

class QAction;
class QActionGroup;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class QLabel;
class QGroupBox;
class QCloseEvent;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    CadDocument document_;
    CadViewport* viewport_{nullptr};
    QTreeWidget* tree_{nullptr};
    QLineEdit* projectName_{nullptr};
    QLineEdit* objectName_{nullptr};
    QComboBox* units_{nullptr};
    QComboBox* material_{nullptr};
    QPushButton* colorButton_{nullptr};
    QGroupBox* primitiveGroup_{nullptr};
    std::array<QDoubleSpinBox*, 3> positionFields_{};
    std::array<QDoubleSpinBox*, 3> rotationFields_{};
    std::array<QDoubleSpinBox*, 3> scaleFields_{};
    std::array<QDoubleSpinBox*, 3> primitiveFields_{};
    std::array<QLabel*, 3> primitiveLabels_{};
    QLabel* areaValue_{nullptr};
    QLabel* volumeValue_{nullptr};
    QLabel* densityValue_{nullptr};
    QLabel* boundsValue_{nullptr};
    QLabel* massValue_{nullptr};
    QLabel* objectCountLabel_{nullptr};
    QLabel* coordinateLabel_{nullptr};
    QAction* undoAction_{nullptr};
    QAction* redoAction_{nullptr};
    std::vector<CadDocument> history_;
    int historyIndex_{-1};
    int savedHistoryIndex_{-1};
    QString projectPath_;
    bool syncingInspector_{false};

    void createInterface();
    void createMenusAndToolbar();
    void createProjectDock();
    void createInspectorDock();
    void connectActions();
    void updateTree();
    void updateInspector(bool updateMeasurements = true);
    void updatePhysicalInspector(const CadObject& object);
    void updateStatus();
    void updateWindowTitle();
    void refreshViewport();
    void pushHistory(bool markSaved = false);
    void restoreHistory(int index);
    bool confirmSaveChanges();
    int selectedIndex() const;
    double unitFactor() const;
    QString uniqueName(const QString& base) const;
    void addPrimitive(const QString& kind);
    void editPrimitiveParameters();
    void importFiles();
    void openProject();
    bool saveProject(bool saveAs = false);
    void exportTo(const QString& extension);
    void newProject();
    void deleteSelected();
    void duplicateSelected();
    void setTool(CadViewport::Tool tool);
    void launchUpdater();
    void showCompatibilityHelp();
    void showAbout();
};
