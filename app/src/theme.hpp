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
inline const char* kBorder = "#3c3c41";   // subtle borders
inline const char* kText = "#d4d4d4";     // primary text
inline const char* kMuted = "#9d9d9d";    // secondary text
inline const char* kSelection = "#264f78"; // selection highlight
inline const char* kHover = "#2a2d2e";    // hover background
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
    color: %9;
    font-weight: 700;
    padding: 6px 8px;
    border-bottom: 1px solid %5;
}
QMainWindow, QDialog {
    background-color: %1;
}
QTableView, QTreeView, QListView, QListWidget {
    background-color: %3;
    alternate-background-color: %4;
    border: 1px solid %5;
    gridline-color: %5;
    selection-background-color: %6;
    selection-color: #ffffff;
    outline: none;
}
QTableView::item, QTreeView::item, QListWidget::item {
    padding: 2px 4px;
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
    padding: 4px 6px;
    font-weight: 600;
}
QLineEdit, QComboBox, QSpinBox, QTextEdit, QPlainTextEdit {
    background-color: %3;
    border: 1px solid %5;
    border-radius: 3px;
    padding: 4px 6px;
    selection-background-color: %6;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
    border: 1px solid %9;
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
    border-radius: 3px;
    padding: 4px 12px;
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
}
QPushButton[accent="true"]:hover {
    background-color: #4fc1d9;
}
QPushButton[danger="true"] {
    background-color: %10;
    color: #1a1a1a;
    font-weight: 600;
    border: none;
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
}
QMenu::item {
    padding: 5px 24px 5px 20px;
}
QMenu::item:selected {
    background-color: %6;
}
QMenu::separator {
    height: 1px;
    background-color: %5;
    margin: 4px 8px;
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
    border-radius: 3px;
    text-align: center;
    color: %2;
}
QProgressBar::chunk {
    background-color: %9;
    border-radius: 2px;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 14px; height: 14px;
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
