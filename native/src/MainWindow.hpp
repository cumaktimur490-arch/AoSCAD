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

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    CadDocument document_;
    CadViewport* viewport_{nullptr};
    QTreeWidget* tree_{nullptr};
    QLineEdit* projectName_{nullptr};
    QLineEdit* objectName_{nullptr};
    QComboBox* units_{nullptr};
    QPushButton* colorButton_{nullptr};
    std::array<QDoubleSpinBox*, 3> positionFields_{};
    std::array<QDoubleSpinBox*, 3> rotationFields_{};
    std::array<QDoubleSpinBox*, 3> scaleFields_{};
    QLabel* objectCountLabel_{nullptr};
    QLabel* coordinateLabel_{nullptr};
    QAction* undoAction_{nullptr};
    QAction* redoAction_{nullptr};
    std::vector<CadDocument> history_;
    int historyIndex_{-1};
    bool syncingInspector_{false};

    void createInterface();
    void createMenusAndToolbar();
    void createProjectDock();
    void createInspectorDock();
    void connectActions();
    void updateTree();
    void updateInspector();
    void updateStatus();
    void refreshViewport();
    void pushHistory();
    void restoreHistory(int index);
    int selectedIndex() const;
    double unitFactor() const;
    QString uniqueName(const QString& base) const;
    void addPrimitive(const QString& kind);
    void importFiles();
    void openProject();
    void saveProject();
    void exportTo(const QString& extension);
    void newProject();
    void deleteSelected();
    void duplicateSelected();
    void setTool(CadViewport::Tool tool);
    void showCompatibilityHelp();
    void showAbout();
};
