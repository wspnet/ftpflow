// ftpflow -- main window: dual-pane, local pane wired to core.lib.
#pragma once

#include <QtWidgets/QMainWindow>

#include "local_fs_model.hpp"

class QLineEdit;
class QTableView;
class QSortFilterProxyModel;
class QLabel;
class QStackedWidget;

namespace ftpflow {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onOpen();
    void onUp();
    void onActivated(const QModelIndex& idx);
    void onVerdictsReady();

private:
    void buildUi();
    void showRoot(const std::string& root);
    void refreshStatusBar();

    LocalFsModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTableView* left_ = nullptr;
    QStackedWidget* right_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
};

}  // namespace ftpflow
