// ftpflow -- self-hosted terminal emulator widget (no third-party deps).
//
// Renders a TerminalBuffer via QPainter and translates keyboard input into
// the byte sequences a remote PTY expects. Data from the remote arrives via
// feed(); keyboard input leaves via the dataReady() signal so the owner can
// write it down the SSH channel.

#pragma once

#include <QWidget>

#include "terminal_buffer.hpp"
#include "vt_parser.hpp"

class QPaintEvent;
class QKeyEvent;
class QResizeEvent;
class QMouseEvent;

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

signals:
    // Keyboard input to send down the SSH channel.
    void dataReady(const QByteArray& data);
    // Emitted after a resize so the owner can request a PTY resize.
    void sizeChanged(int cols, int rows);

protected:
    void paintEvent(QPaintEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void recomputeGeometry();
    void sendString(const QByteArray& s);
    QColor resolveColor(const TermColor& c, bool fg) const;

    TerminalBuffer buffer_;
    VtParser parser_;

    QFont font_;
    int cellW_ = 8;      // character cell width in pixels
    int cellH_ = 15;     // character cell height in pixels
    int fontDescent_ = 3; // font baseline offset from cell top
    bool geometryDirty_ = true;
};

}  // namespace ftpflow
