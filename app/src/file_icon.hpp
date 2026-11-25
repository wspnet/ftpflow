// ftpflow -- file-type icons for the local/remote file tables.
//
// A tiny, dependency-free icon factory that paints flat, theme-aware icons
// with QPainter (no image assets needed). Icons are keyed by extension +
// entry type and cached so a 100k-row listing stays cheap.
#pragma once

#include <QtGui/QIcon>

#include "ftpflow/vfs.hpp"

namespace ftpflow {

// Returns the cached icon for `entry` (directory vs. file, and for files the
// extension). Thread-safe for reads; first paint is lazy.
QIcon fileIcon(const Entry& entry);

// Explicit icon for a directory (used when only a name/type is known).
QIcon folderIcon();

// Pre-warm the cache for common extensions (avoids first-paint jank).
void prewarmFileIcons();

}  // namespace ftpflow
