// ftpflow -- application-wide dark theme (XTerminal-inspired).
//
// A single entry point applies a cohesive dark look: a QPalette for native
// chrome (menus, scrollbars, tooltips, dialogs) plus a QSS stylesheet for
// widgets (panes, tables, buttons, inputs, docks). Kept in one file so the
// whole shell shares the same palette and can be themed later.
#pragma once

#include <QtGui/QPalette>
#include <QtWidgets/QApplication>

#include <QString>

namespace ftpflow {

// Core palette (dark, low-chroma, high-contrast).
inline const char* kAccent = "#2ea8c0";   // teal accent (XTerminal-like)
inline const char* kBg = "#1e1e1e";       // window background
inline const char* kPanel = "#252526";    // panels / docks / headers
inline const char* kField = "#2d2d30";    // inputs / table background
inline const char* kBorder = "#38383d";   // subtle borders
inline const char* kText = "#d4d4d4";     // primary text
inline const char* kMuted = "#9d9d9d";    // secondary text
inline const char* kSelection = "#264f78"; // selection highlight
inline const char* kHover = "#2f3134";    // hover background
inline const char* kDanger = "#f48771";   // error / destructive

// Build the application palette.
inline QPalette darkPalette() {
    QPalette p;
    const QColor bg(kBg), text(kText), muted(kMuted), accent(kAccent),
        panel(kPanel), field(kField), border(kBorder), sel(kSelection),
        hover(kHover);

    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, field);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, muted);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor("#ffffff"));
    p.setColor(QPalette::Highlight, sel);
    p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor("#6a6a6a"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#6a6a6a"));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#6a6a6a"));

    // Group boxes and mid-lines.
    p.setColor(QPalette::Mid, border);
    p.setColor(QPalette::Dark, QColor("#151515"));
    p.setColor(QPalette::Shadow, QColor("#000000"));
    return p;
}

// Global QSS stylesheet.
inline QString darkStyleSheet() {
    return QStringLiteral(R"QSS(
QWidget {
    background-color: %1;
    color: %2;
    font-size: 9pt;
}
QLabel#paneTitle {
    background-color: %4;
    color: %2;
    font-weight: 700;
    padding: 8px 12px;
    border-radius: 6px;
}
QMainWindow, QDialog {
    background-color: %1;
}
QWidget#titleBar {
    background-color: %4;
}
QLabel#titleBarText {
    background: transparent;
    color: %9;
    font-weight: 600;
    padding-left: 6px;
}
QToolButton#titleBarBtn {
    background: transparent;
    border: none;
    border-radius: 4px;
    margin: 2px;
}
QToolButton#titleBarBtn:hover {
    background-color: %7;
    border-radius: 4px;
}
QToolButton#titleBarCloseBtn:hover {
    background-color: #c42b1c;
}
QToolButton#accountBtn {
    background: transparent;
    border: none;
    border-radius: 6px;
    color: %8;
    padding: 2px 10px;
}
QToolButton#accountBtn:hover {
    background-color: %7;
    color: %2;
}
QToolButton#accountBtn::menu-indicator {
    image: none;
}

/* ---- Account menu (simple header + plain items) ---- */
QWidget#accountMenuHeader {
    background: transparent;
}
QLabel#accountMenuName {
    background: transparent;
    color: %2;
    font-size: 11pt;
    font-weight: 700;
}
QLabel#accountMenuBadge {
    background: transparent;
    color: %8;
    font-size: 8.5pt;
}

QTableView, QTreeView, QListView, QListWidget {
    background-color: %3;
    alternate-background-color: %4;
    border: 1px solid %5;
    border-radius: 6px;
    gridline-color: %5;
    selection-background-color: %6;
    selection-color: #ffffff;
    outline: none;
}
QTableView::item, QTreeView::item, QListWidget::item {
    padding: 3px 6px;
}
QTableView::item:hover, QTreeView::item:hover, QListWidget::item:hover {
    background-color: %7;
}
QHeaderView::section {
    background-color: %4;
    color: %8;
    border: none;
    border-right: 1px solid %5;
    border-bottom: 1px solid %5;
    padding: 5px 8px;
    font-weight: 600;
}
QLineEdit, QComboBox, QSpinBox, QTextEdit, QPlainTextEdit {
    background-color: %3;
    border: 1px solid %5;
    border-radius: 6px;
    padding: 5px 8px;
    selection-background-color: %6;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
    border: 1px solid %9;
}
/* Inline rename editor lives inside a ~20px table row; the generic 5px
   vertical padding above would squeeze the text out of view. */
QLineEdit[ftpflowInlineEdit="true"] {
    padding: 0px 4px;
    border-radius: 3px;
}
QLineEdit[readOnly="true"] {
    background-color: %1;
    color: %8;
}
QComboBox::drop-down, QSpinBox::up-button, QSpinBox::down-button {
    border: none;
    width: 16px;
}
QComboBox QAbstractItemView {
    background-color: %3;
    border: 1px solid %5;
    selection-background-color: %6;
}
QPushButton {
    background-color: %4;
    border: 1px solid %5;
    border-radius: 6px;
    padding: 5px 14px;
}
QPushButton:hover {
    background-color: %7;
    border: 1px solid %9;
}
QPushButton:pressed {
    background-color: %6;
}
QPushButton:disabled {
    color: #6a6a6a;
    background-color: %1;
    border: 1px solid %5;
}
QPushButton[accent="true"] {
    background-color: %9;
    color: #0b0c0d;
    font-weight: 600;
    border: none;
    border-radius: 6px;
}
QPushButton[accent="true"]:hover {
    background-color: #4fc1d9;
}
QPushButton[danger="true"] {
    background-color: %10;
    color: #1a1a1a;
    font-weight: 600;
    border: none;
    border-radius: 6px;
}
QMenuBar {
    background-color: %4;
    border-bottom: 1px solid %5;
}
QMenuBar::item:selected {
    background-color: %7;
}
QMenu {
    background-color: %4;
    border: 1px solid %5;
    border-radius: 6px;
    padding: 4px;
}
QMenu::item {
    padding: 5px 24px 5px 20px;
    border-radius: 4px;
}
QMenu::item:selected {
    background-color: %6;
}
QMenu::separator {
    height: 1px;
    background-color: %5;
    margin: 4px 8px;
}
QTabWidget::pane {
    background-color: %3;
    border-left: 1px solid %5;
    border-right: 1px solid %5;
    border-bottom: 1px solid %5;
    border-top: none;
    border-bottom-left-radius: 6px;
    border-bottom-right-radius: 6px;
}
QTabBar {
    background: transparent;
    border: none;
}
QTabBar::tab {
    background: transparent;
    color: %8;
    border: none;
    border-radius: 8px;
    padding: 6px 12px;
    margin-right: 4px;
}
QTabBar::tab:hover:!selected {
    background-color: %7;
}
QTabBar::tab:selected {
    background-color: #3a3d41;
    color: #ffffff;
    font-weight: 600;
}
QTabBar::tab:last {
    /* The "+" new-session tab: a compact rounded square button with a small
       gap before it, matching the modern browser new-tab button. */
    padding: 6px 7px;
    margin-left: 4px;
    background: transparent;
    color: %2;
}
QTabBar::tab:last:hover {
    background-color: %7;
}
QToolButton#tabCloseBtn {
    background: transparent;
    border: none;
    border-radius: 4px;
    padding: 2px;
    margin: 0px;
}
QToolButton#tabCloseBtn:hover {
    background-color: %7;
}
QToolButton#tabCloseBtn:pressed {
    background-color: %6;
}
QDockWidget {
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background-color: %4;
    padding: 6px;
    border-bottom: 1px solid %5;
    font-weight: 600;
}
QWidget#dockTitleBar {
    background-color: %4;
    border-bottom: 1px solid %5;
}
QToolButton#dockCloseBtn {
    background: transparent;
    border: none;
    border-radius: 4px;
    margin: 2px;
}
QToolButton#dockCloseBtn:hover {
    background-color: #c42b1c;
}
QStatusBar {
    background-color: %4;
    border-top: 1px solid %5;
    color: %8;
}
QStatusBar QLabel {
    color: %8;
}
QToolTip {
    background-color: %4;
    color: %2;
    border: 1px solid %5;
    border-radius: 4px;
    padding: 4px;
}
QSplitter::handle {
    background-color: %5;
}
QSplitter::handle:hover {
    background-color: %9;
}
QScrollBar:vertical {
    background: %1;
    width: 12px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: %5;
    border-radius: 5px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover {
    background: #5a5a5f;
}
QScrollBar:horizontal {
    background: %1;
    height: 12px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: %5;
    border-radius: 5px;
    min-width: 24px;
}
QScrollBar::handle:horizontal:hover {
    background: #5a5a5f;
}
QScrollBar::add-line, QScrollBar::sub-line {
    height: 0; width: 0;
}
QProgressBar {
    background-color: %3;
    border: 1px solid %5;
    border-radius: 6px;
    text-align: center;
    color: %2;
}
QProgressBar::chunk {
    background-color: %9;
    border-radius: 5px;
}
QCheckBox, QRadioButton {
    color: %2;
    spacing: 6px;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 16px; height: 16px;
    background-color: %3;
    border: 1px solid %8;
    border-radius: 3px;
}
QCheckBox::indicator:hover, QRadioButton::indicator:hover {
    border: 1px solid %9;
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background-color: %9;
    border: 1px solid %9;
}
)QSS")
        .arg(kBg, kText, kField, kPanel, kBorder, kSelection, kHover,
             kMuted, kAccent, kDanger);
}

// Apply the dark theme to the running application.
inline void applyDarkTheme(QApplication& app) {
    app.setPalette(darkPalette());
    app.setStyleSheet(darkStyleSheet());
}

}  // namespace ftpflow
