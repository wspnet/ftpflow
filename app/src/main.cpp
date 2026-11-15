// ftpflow -- Qt Widgets application entry point.
#include "mainwindow.hpp"

#include <QtWidgets/QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ftpflow");
    QApplication::setOrganizationName("ftpflow");

    ftpflow::MainWindow win;
    win.resize(1180, 720);
    win.show();
    return QApplication::exec();
}
