// ftpflow -- Qt Widgets application entry point.
#include "mainwindow.hpp"
#include "file_icon.hpp"
#include "theme.hpp"

#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

#include <QEvent>
#include <QObject>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#endif

namespace {

// Applies Windows 11 native rounded corners to every top-level dialog that
// pops up during the session -- QMessageBox, QFileDialog, QInputDialog, etc.
// The main window and the connect dialog do this themselves in showEvent();
// this filter catches the system-styled message boxes that would otherwise
// keep their square corners and look inconsistent with the shell.
class RoundedCornerFilter : public QObject {
public:
    bool eventFilter(QObject* obj, QEvent* event) override {
#ifdef Q_OS_WIN
        if (event->type() == QEvent::Show) {
            auto* w = qobject_cast<QWidget*>(obj);
            if (w && w->isWindow() && w != &exclude_) {
                const HWND hwnd = reinterpret_cast<HWND>(w->winId());
                const DWM_WINDOW_CORNER_PREFERENCE pref =
                    static_cast<DWM_WINDOW_CORNER_PREFERENCE>(DWMWCP_ROUND);
                DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                      &pref, sizeof(pref));
            }
        }
#else
        (void)obj; (void)event;
#endif
        return QObject::eventFilter(obj, event);
    }
    // The main window (and any window already handling corners itself) is
    // skipped -- it already calls DwmSetWindowAttribute in showEvent().
    QWidget& exclude_;
    explicit RoundedCornerFilter(QWidget& exclude) : exclude_(exclude) {}
};

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ftpflow");
    QApplication::setOrganizationName("ftpflow");

    // A consistent, XTerminal-inspired dark theme across all platforms.
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    ftpflow::applyDarkTheme(app);
    ftpflow::prewarmFileIcons();

    ftpflow::MainWindow win;
    win.resize(1180, 720);

    // Round the corners of every dialog that opens during the session so the
    // whole app matches (message boxes, file dialogs, input dialogs, ...).
    static RoundedCornerFilter rounded(win);
    app.installEventFilter(&rounded);

    win.show();
    return QApplication::exec();
}
