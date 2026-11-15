// ftpflow -- main window: dual-pane scaffold, wired to core.lib only.
#include "mainwindow.hpp"

#include <QtWidgets/QLabel>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTreeView>

#include "ftpflow/version.hpp"

namespace ftpflow {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle(QString("ftpflow %1 - file transfer workbench")
                       .arg(versionString()));
}

void MainWindow::buildUi() {
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    left_ = new QTreeView(splitter);
    right_ = new QTreeView(splitter);
    left_->setHeaderHidden(false);
    right_->setHeaderHidden(false);
    // M1: attach QAbstractItemModel backed by LocalFsBackend::list() pages
    // here; uniformRowHeights stays on for the 100k-row budget.
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    auto* status = new QLabel(
        "skeleton: model wiring lands in M1 (blueprint section 07)", this);
    statusBar()->addWidget(status);
}

}  // namespace ftpflow
