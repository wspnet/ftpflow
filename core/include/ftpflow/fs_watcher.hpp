// ftpflow -- ReadDirectoryChangesW-based change watcher.
// Part of core.lib: pure C++17 + Win32, no Qt.
//
// One background thread; one OVERLAPPED ReadDirectoryChangesW per watched
// directory (non-recursive). Any event in a watched directory -- create,
// write, rename, delete, or a buffer overflow -- fires the callback with
// the directory path. The typical consumer is DirCache::invalidate(). The
// callback runs on the watcher thread, so consumers must be thread-safe.
//
// Watch cap: 60 directories (WaitForMultipleObjects is limited to 64
// handles: stop + wakeup + watches). Exceeding the cap makes watch() a
// no-op that returns false; callers should treat unwatched directories as
// always-stale.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ftpflow {

class FsWatcher {
public:
    using ChangeCallback = std::function<void(const std::string& dir)>;

    explicit FsWatcher(ChangeCallback onChange);
    ~FsWatcher();  // stops the thread and closes all watches

    FsWatcher(const FsWatcher&) = delete;
    FsWatcher& operator=(const FsWatcher&) = delete;

    // Idempotent. Returns false when the cap is hit or the directory
    // cannot be opened for change notification.
    bool watch(const std::string& dir);

    void unwatch(const std::string& dir);
    void unwatchAll();
    std::size_t watchedCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ftpflow
