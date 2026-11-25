// ftpflow -- file-type icons implementation.
#include "file_icon.hpp"

#include <QtCore/QHash>
#include <QtCore/QMutex>
#include <QtCore/QString>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>
#include <QtGui/QPixmap>

#include <algorithm>
#include <cctype>

namespace ftpflow {
namespace {

constexpr int kIconSize = 16;

// ---- palette (matches the XTerminal dark theme accents) --------------------
const QColor kDirFill("#e0a93e");        // folder: amber
const QColor kDirFold("#b8862b");        // folder: darker front face
const QColor kImg("#5abf8a");            // images: green
const QColor kVideo("#a06bd8");          // video: violet
const QColor kAudio("#4fc1d9");          // audio: teal
const QColor kArchive("#e08a3e");        // archives: orange
const QColor kCode("#5b8fe0");           // code: blue
const QColor kText("#9d9d9d");           // text/docs: muted grey
const QColor kPdf("#e0524f");            // pdf: red
const QColor kDoc("#4f86d9");            // word: blue
const QColor kSheet("#3fa05a");          // excel: green
const QColor kSlides("#e06a4f");         // powerpoint: orange-red
const QColor kExe("#2ea8c0");            // executable: teal accent
const QColor kGeneric("#8a8a8a");        // unknown: grey

// ---- tiny helpers ----------------------------------------------------------

std::string lowerExt(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// Returns a category key for a file extension. Empty -> generic file.
const char* categoryForKey(const std::string& ext) {
    if (ext.empty()) return "generic";
    // Images.
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
        ext == "bmp" || ext == "webp" || ext == "svg" || ext == "ico" ||
        ext == "tif" || ext == "tiff" || ext == "heic" || ext == "avif")
        return "image";
    // Video.
    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" ||
        ext == "wmv" || ext == "flv" || ext == "webm" || ext == "m4v" ||
        ext == "mpg" || ext == "mpeg" || ext == "ts")
        return "video";
    // Audio.
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "aac" ||
        ext == "ogg" || ext == "m4a" || ext == "wma" || ext == "opus" ||
        ext == "mid" || ext == "midi")
        return "audio";
    // Archives.
    if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" ||
        ext == "gz" || ext == "tgz" || ext == "bz2" || ext == "xz" ||
        ext == "zst" || ext == "cab" || ext == "iso" || ext == "jar")
        return "archive";
    // Code.
    if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" ||
        ext == "hpp" || ext == "c" || ext == "cs" || ext == "java" ||
        ext == "py" || ext == "js" || ext == "ts" || ext == "jsx" ||
        ext == "tsx" || ext == "go" || ext == "rs" || ext == "rb" ||
        ext == "php" || ext == "swift" || ext == "kt" || ext == "sh" ||
        ext == "bat" || ext == "ps1" || ext == "lua" || ext == "pl" ||
        ext == "sql" || ext == "html" || ext == "css" || ext == "scss" ||
        ext == "vue" || ext == "dart" || ext == "m" || ext == "mm")
        return "code";
    // Documents.
    if (ext == "pdf") return "pdf";
    if (ext == "doc" || ext == "docx" || ext == "rtf" || ext == "odt")
        return "doc";
    if (ext == "xls" || ext == "xlsx" || ext == "csv" || ext == "ods")
        return "sheet";
    if (ext == "ppt" || ext == "pptx" || ext == "odp") return "slides";
    if (ext == "txt" || ext == "md" || ext == "markdown" || ext == "log" ||
        ext == "json" || ext == "xml" || ext == "yaml" || ext == "yml" ||
        ext == "ini" || ext == "conf" || ext == "toml" || ext == "cfg")
        return "text";
    // Executables.
    if (ext == "exe" || ext == "msi" || ext == "dll" || ext == "app" ||
        ext == "bin" || ext == "deb" || ext == "rpm" || ext == "apk")
        return "exe";
    return "generic";
}

QColor colorForKey(const char* cat) {
    const std::string c(cat);
    if (c == "image") return kImg;
    if (c == "video") return kVideo;
    if (c == "audio") return kAudio;
    if (c == "archive") return kArchive;
    if (c == "code") return kCode;
    if (c == "pdf") return kPdf;
    if (c == "doc") return kDoc;
    if (c == "sheet") return kSheet;
    if (c == "slides") return kSlides;
    if (c == "exe") return kExe;
    if (c == "text") return kText;
    return kGeneric;
}

// A short 1-3 char glyph shown inside the file icon (extension-derived).
QString glyphForKey(const char* cat, const std::string& ext) {
    const std::string c(cat);
    if (c == "image") return "IMG";
    if (c == "video") return "VID";
    if (c == "audio") return "AUD";
    if (c == "archive") return "ZIP";
    if (c == "code") return "{}";
    if (c == "pdf") return "PDF";
    if (c == "doc") return "DOC";
    if (c == "sheet") return "XLS";
    if (c == "slides") return "PPT";
    if (c == "exe") return "EXE";
    if (c == "text") return "TXT";
    // Generic: use the uppercased extension if short enough.
    if (!ext.empty() && ext.size() <= 4) {
        QString g = QString::fromStdString(ext).toUpper();
        return g;
    }
    return "?";
}

// ---- painters ---------------------------------------------------------------

QPixmap paintFolderIcon() {
    QPixmap pm(kIconSize, kIconSize);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Folder tab (back flap).
    p.setPen(Qt::NoPen);
    p.setBrush(kDirFold);
    p.drawRoundedRect(QRectF(1.0, 3.0, 6.0, 3.0), 1.0, 1.0);
    // Folder body.
    p.setBrush(kDirFill);
    p.drawRoundedRect(QRectF(1.0, 5.0, 14.0, 9.0), 1.5, 1.5);

    return pm;
}

QPixmap paintFileIcon(const char* cat, const std::string& ext) {
    const QColor color = colorForKey(cat);
    const QString glyph = glyphForKey(cat, ext);

    QPixmap pm(kIconSize, kIconSize);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Document sheet with a folded corner.
    QPainterPath sheet;
    sheet.moveTo(2.0, 1.0);
    sheet.lineTo(11.0, 1.0);
    sheet.lineTo(14.0, 4.0);
    sheet.lineTo(14.0, 15.0);
    sheet.lineTo(2.0, 15.0);
    sheet.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(sheet);

    // Folded corner (slightly darker).
    QColor dark = color.darker(125);
    QPainterPath fold;
    fold.moveTo(11.0, 1.0);
    fold.lineTo(14.0, 4.0);
    fold.lineTo(11.0, 4.0);
    fold.closeSubpath();
    p.setBrush(dark);
    p.drawPath(fold);

    // Glyph.
    QFont f = p.font();
    f.setPixelSize(7);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor("#141414"));
    p.drawText(QRectF(2.0, 5.0, 12.0, 10.0), Qt::AlignCenter, glyph);

    return pm;
}

// ---- cache ------------------------------------------------------------------

QMutex g_mutex;
QHash<QString, QIcon> g_cache;

QIcon iconFor(const char* cat, const std::string& ext) {
    const QString key = QString::fromLatin1(cat) + ":" +
                        QString::fromStdString(ext);
    {
        QMutexLocker lock(&g_mutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it.value();
    }
    QIcon icon(paintFileIcon(cat, ext));
    QMutexLocker lock(&g_mutex);
    g_cache.insert(key, icon);
    return icon;
}

}  // namespace

QIcon folderIcon() {
    const QString key = QStringLiteral("__folder__");
    {
        QMutexLocker lock(&g_mutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it.value();
    }
    QIcon icon(paintFolderIcon());
    QMutexLocker lock(&g_mutex);
    g_cache.insert(key, icon);
    return icon;
}

QIcon fileIcon(const Entry& entry) {
    if (entry.type == EntryType::Directory) return folderIcon();
    if (entry.type == EntryType::Symlink) return folderIcon();
    const std::string ext = lowerExt(entry.name);
    return iconFor(categoryForKey(ext), ext);
}

void prewarmFileIcons() {
    static const char* exts[] = {
        "png", "jpg", "mp4", "mp3", "zip", "cpp", "h", "py", "js", "pdf",
        "docx", "xlsx", "pptx", "txt", "md", "json", "exe", "", nullptr,
    };
    for (int i = 0; exts[i]; ++i) {
        const std::string ext = exts[i];
        const char* cat = categoryForKey(ext);
        (void)iconFor(cat, ext);
    }
}

}  // namespace ftpflow
