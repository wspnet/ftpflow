// ftpflow -- self-hosted terminal emulator widget (no third-party deps).
//
// Renders a TerminalBuffer via QPainter and translates keyboard input into
// the byte sequences a remote PTY expects. Data from the remote arrives via
// feed(); keyboard input leaves via the dataReady() signal so the owner can
// write it down the SSH channel.
//
// Convenience features modeled on mainstream terminals (Windows Terminal /
// VS Code / PuTTY): Ctrl+C/V copy & paste (Ctrl+C falls back to SIGINT),
// scrollback history with wheel/Shift+PgUp, font zoom, triple-click line
// selection, middle-click paste, and a find bar (Ctrl+Shift+F).

#pragma once

#include <QWidget>
#include <vector>

#include "terminal_buffer.hpp"
#include "vt_parser.hpp"

class QPaintEvent;
class QKeyEvent;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QMouseEvent;
class QContextMenuEvent;
class QWheelEvent;
class QFocusEvent;
class QLineEdit;
class QLabel;
class QToolButton;

namespace ftpflow {

class TerminalWidget : public QWidget {
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget* parent = nullptr);

    // Feed raw PTY output into the emulator (called from the network thread
    // via a queued signal).
    void feed(const QByteArray& data);

    // Terminal dimensions in cells.
    int columns() const { return buffer_.cols(); }
    int rows() const { return buffer_.rows(); }

    // Font zoom in points (conventional Ctrl+= / Ctrl+- / Ctrl+0).
    void zoomBy(int deltaPt);
    void resetZoom();

    // Toggle the on-screen soft keyboard (arrow up / down / enter). It is a
    // floating button cluster for touch / phone remote-control where a
    // physical keyboard is unavailable. Shown by default once the terminal is
    // visible; callers can hide it if they prefer.
    void setSoftKeyboardVisible(bool visible);

    // Character encoding of the PTY byte stream. The remote side may emit
    // UTF-8 (modern Linux) or GBK (zh_CN.GBK locales / GBK-encoded names);
    // the byte stream is decoded to code points before parsing. Auto probes
    // each chunk (valid UTF-8 -> UTF-8, otherwise GBK) with split-sequence
    // carry-over across feed() calls.
    enum class TermEncoding { Auto, Utf8, Gbk };
    void setEncoding(TermEncoding enc);
    TermEncoding encoding() const { return encoding_; }

signals:
    // Keyboard input to send down the SSH channel.
    void dataReady(const QByteArray& data);
    // Emitted after a resize so the owner can request a PTY resize.
    void sizeChanged(int cols, int rows);
    // OSC 0/2 window-title change reported by the remote.
    void titleChanged(const QString& title);

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    void recomputeGeometry();
    void applyFontMetrics();
    void sendString(const QByteArray& s);
    void sendText(const QString& text);   // encoded per effective input encoding
    void positionSoftKeys();              // pin the soft-key cluster bottom-right
    QByteArray decodeForParser(const QByteArray& raw);
    static int incompleteUtf8Tail(const QByteArray& buf);
    static int completeGbkPrefix(const QByteArray& buf);
    QColor resolveColor(const TermColor& c, bool fg, bool bold) const;

    // --- scrollback view ---------------------------------------------------
    int historySize() const;
    int maxViewOffset() const;
    int globalRowOf(int viewRow) const;         // visible row -> global row
    TermCell cellAtGlobal(int col, int grow) const;   // blank-padded
    void scrollBy(int lines);                   // + = back in history
    void scrollToBottom();

    // --- text selection (global row coordinates) ---------------------------
    TermPos posAt(const QPoint& pt) const;   // pixel -> cell (clamped)
    void setSelectionAnchor(const TermPos& p);
    void updateSelection(const TermPos& p);
    void clearSelection();
    bool hasSelection() const { return selVisible_ && selAnchor_.row >= 0; }
    bool isSelected(int col, int grow) const;
    QString selectedText() const;            // ordered, trailing blanks trimmed
    void copySelection();                    // -> system clipboard
    void pasteClipboard();                   // clipboard -> remote PTY
    void selectWordAt(const TermPos& p);     // double-click word selection
    void selectLineAt(const TermPos& p);     // triple-click line selection
    void selectAll();                        // history + screen

    // --- find bar -----------------------------------------------------------
    void toggleFindBar();
    void hideFindBar();
    void updateFindMatches(bool resetCurrent);
    void gotoMatch(int delta);
    void updateFindLabel();
    QString lineText(int grow) const;

    struct FindMatch { int row; int col; int len; };

    TerminalBuffer buffer_;
    VtParser parser_;

    QFont font_;
    int fontPt_ = 10;
    int cellW_ = 8;      // character cell width in pixels
    int cellH_ = 15;     // character cell height in pixels
    int fontDescent_ = 3; // font baseline offset from cell top

    // Scrollback: lines scrolled up from the live view (0 = following).
    int viewOffset_ = 0;

    // Encoding layer state.
    TermEncoding encoding_ = TermEncoding::Auto;
    QByteArray encCarry_;               // split multi-byte sequence across feeds
    bool lastOutputWasGbk_ = false;     // effective decision (drives input)

    // Selection state (global row coordinates; -1 row = no selection).
    TermPos selAnchor_{-1, -1};
    TermPos selPos_{-1, -1};
    bool selecting_ = false;
    bool selVisible_ = false;   // highlight only after a real drag / word select
    bool afterDoubleClick_ = false;
    unsigned long long dblClickTs_ = 0;

    // Find bar (child overlay).
    QWidget* findBar_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QLabel* findCount_ = nullptr;
    std::vector<FindMatch> matches_;
    int findCurrent_ = -1;

    // On-screen soft keyboard (arrow up / down / enter) for touch use.
    QWidget* softKeys_ = nullptr;
    bool softKeysVisible_ = true;   // shown while the terminal is displayed
};

}  // namespace ftpflow
