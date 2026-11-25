// ftpflow -- tiny UI string helpers shared across the app models.
#pragma once

#include <QString>

#include <cstdint>
#include <string>

namespace ftpflow {

// Human-readable byte size ("1.5 MiB"). One copy for both panes so the
// local and remote listings always format sizes identically.
inline QString humanSize(std::uint64_t n) {
    if (n < 1024) return QString::number(static_cast<qulonglong>(n)) + " B";
    const double kb = n / 1024.0;
    if (kb < 1024.0) return QString::number(kb, 'f', 1) + " KiB";
    const double mb = kb / 1024.0;
    if (mb < 1024.0) return QString::number(mb, 'f', 1) + " MiB";
    return QString::number(mb / 1024.0, 'f', 2) + " GiB";
}

// Last path component of a '/'- or '\\'-separated path ("" when the path
// ends with a separator or is empty). Works for both local and remote paths.
inline std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Validate a name typed into an inline rename editor. It must be a single
// path component on both Windows and FTP/SFTP servers, so separators, the
// Windows-reserved characters, control characters and the "."/".." aliases
// are rejected. Shared by the local and remote models so both panes behave
// identically.
inline bool isValidEntryName(const QString& name) {
    const QString n = name.trimmed();
    if (n.isEmpty() || n == QStringLiteral(".") ||
        n == QStringLiteral("..") || n.size() > 255)
        return false;
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    for (const QChar c : n) {
        if (c.unicode() < 0x20 || forbidden.contains(c)) return false;
    }
    // Windows rejects trailing dots/spaces; harmless to reject on servers.
    if (n.endsWith(QLatin1Char(' ')) || n.endsWith(QLatin1Char('.')))
        return false;
    return true;
}

}  // namespace ftpflow
