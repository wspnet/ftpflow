// ftpflow -- Qt Widgets application entry point.
#include "mainwindow.hpp"
#include "theme.hpp"

#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ftpflow");
    QApplication::setOrganizationName("ftpflow");

    // A consistent, XTerminal-inspired dark theme across all platforms.
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    ftpflow::applyDarkTheme(app);

    ftpflow::MainWindow win;
    win.resize(1180, 720);
    win.show();
    return QApplication::exec();
}
