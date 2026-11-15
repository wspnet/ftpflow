// ftpflow -- main window: dual-pane scaffold, wired to core.lib only.
#pragma once

#include <QtWidgets/QMainWindow>

class QTreeView;

namespace ftpflow {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildUi();
    QTreeView* left_ = nullptr;
    QTreeView* right_ = nullptr;
};

}  // namespace ftpflow
