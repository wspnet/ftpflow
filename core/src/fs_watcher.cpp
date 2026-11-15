// ftpflow -- ReadDirectoryChangesW watcher implementation.
#include "ftpflow/fs_watcher.hpp"

#include <windows.h>

#include <cstring>

namespace ftpflow {

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

constexpr std::size_t kMaxWatches = 60;
constexpr DWORD kBufBytes = 64 * 1024;

}  // namespace

struct FsWatcher::Impl {
    struct Watch {
        std::string dir;
        HANDLE hDir = INVALID_HANDLE_VALUE;
        OVERLAPPED ov{};          // ov.hEvent is the wake event
        std::vector<BYTE> buf;
    };

    // Watch handles are NEVER closed while the thread runs: WaitForMultiple
    // Objects on a closed handle is WAIT_FAILED and would kill the whole
    // watcher. Removed watches move to `retired` and are reaped in ~Impl
    // after the thread has joined. (Their events may still be in the
    // thread's local array for one cycle; the live-check discards them.)
    ChangeCallback onChange;
    std::mutex mu;               // guards watches/retired/stopping
    std::vector<Watch*> watches;
    std::vector<Watch*> retired;
    bool stopping = false;
    HANDLE stopEvt = nullptr;    // manual-reset
    HANDLE wakeEvt = nullptr;    // auto-reset: watch list changed
    HANDLE thread = nullptr;

    explicit Impl(ChangeCallback cb) : onChange(std::move(cb)) {
        stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wakeEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread = CreateThread(nullptr, 0, &Impl::threadTramp, this, 0, nullptr);
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> g(mu);
            stopping = true;
        }
        SetEvent(stopEvt);
        if (thread) {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
        for (Watch* w : watches) { reap(w); delete w; }
        watches.clear();
        for (Watch* w : retired) { reap(w); delete w; }
        retired.clear();
        if (stopEvt) CloseHandle(stopEvt);
        if (wakeEvt) CloseHandle(wakeEvt);
    }

    static void reap(Watch* w) {
        if (w->hDir != INVALID_HANDLE_VALUE) {
            CancelIoEx(w->hDir, &w->ov);
            CloseHandle(w->hDir);
            w->hDir = INVALID_HANDLE_VALUE;
        }
        if (w->ov.hEvent) {
            CloseHandle(w->ov.hEvent);
            w->ov.hEvent = nullptr;
        }
    }

    // Caller holds `mu` and has already reset ov.hEvent.
    static bool armWatch(Watch* w) {
        return ReadDirectoryChangesW(w->hDir, w->buf.data(),
                                     static_cast<DWORD>(w->buf.size()),
                                     FALSE,   // non-recursive
                                     FILE_NOTIFY_CHANGE_FILE_NAME |
                                         FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                         FILE_NOTIFY_CHANGE_SIZE |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE,
                                     nullptr, &w->ov, nullptr) != FALSE;
    }

    // Detach from the active list; caller keeps `mu` held.
    void retireLocked(Watch* w) {
        for (auto it = watches.begin(); it != watches.end(); ++it) {
            if (*it == w) { watches.erase(it); break; }
        }
        retired.push_back(w);
        SetEvent(wakeEvt);
    }

    static DWORD WINAPI threadTramp(void* self) {
        static_cast<Impl*>(self)->run();
        return 0;
    }

    void run() {
        for (;;) {
            // Snapshot handles + watches under the lock (WFMO limit 64).
            HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
            Watch* byIndex[MAXIMUM_WAIT_OBJECTS] = {};
            DWORD n = 0;
            handles[n] = stopEvt; ++n;
            handles[n] = wakeEvt; ++n;
            {
                std::lock_guard<std::mutex> g(mu);
                for (Watch* w : watches) {
                    if (n >= MAXIMUM_WAIT_OBJECTS) break;
                    handles[n] = w->ov.hEvent;
                    byIndex[n] = w;
                    ++n;
                }
            }
            DWORD rc = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
            if (rc == WAIT_FAILED) { Sleep(20); continue; }  // never die on it
            DWORD idx = rc - WAIT_OBJECT_0;
            if (idx == 0) break;                       // stop
            if (idx == 1) continue;                    // watch list changed

            Watch* w = byIndex[idx];
            std::string changedDir;
            {
                std::lock_guard<std::mutex> g(mu);
                if (stopping) break;
                // Still active? (unwatch may have retired it between
                // building the array and now.)
                bool live = false;
                for (Watch* x : watches) { if (x == w) { live = true; break; } }
                if (!live) continue;
                changedDir = w->dir;
                DWORD bytes = 0;
                if (!GetOverlappedResult(w->hDir, &w->ov, &bytes, FALSE)) {
                    retireLocked(w);                   // dir deleted etc.
                } else {
                    // bytes == 0 -> buffer overflow: treat as changed too.
                    ResetEvent(w->ov.hEvent);
                    if (!armWatch(w)) retireLocked(w);
                }
            }
            if (onChange) onChange(changedDir);
        }
    }
};

FsWatcher::FsWatcher(ChangeCallback onChange)
    : impl_(std::make_unique<Impl>(std::move(onChange))) {}

FsWatcher::~FsWatcher() = default;

bool FsWatcher::watch(const std::string& dir) {
    if (dir.empty() || !impl_->onChange) return false;
    std::lock_guard<std::mutex> g(impl_->mu);
    for (const Impl::Watch* w : impl_->watches) {
        if (w->dir == dir) return true;  // idempotent
    }
    if (impl_->watches.size() >= kMaxWatches) return false;

    HANDLE hDir = CreateFileW(utf8ToWide(dir).c_str(), FILE_LIST_DIRECTORY,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS |
                                  FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (hDir == INVALID_HANDLE_VALUE) return false;

    auto* w = new Impl::Watch();
    w->dir = dir;
    w->hDir = hDir;
    w->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual
    w->buf.assign(kBufBytes, 0);
    if (!w->ov.hEvent || !Impl::armWatch(w)) {
        // Failed before ever joining `watches`; no other thread can see the
        // handle, so reap inline instead of retiring it.
        SetEvent(impl_->wakeEvt);
        Impl::reap(w);
        delete w;
        return false;
    }
    impl_->watches.push_back(w);
    SetEvent(impl_->wakeEvt);
    return true;
}

void FsWatcher::unwatch(const std::string& dir) {
    std::lock_guard<std::mutex> g(impl_->mu);
    for (auto it = impl_->watches.begin(); it != impl_->watches.end(); ++it) {
        if ((*it)->dir == dir) {
            impl_->retireLocked(*it);
            return;
        }
    }
}

void FsWatcher::unwatchAll() {
    std::lock_guard<std::mutex> g(impl_->mu);
    while (!impl_->watches.empty()) {
        impl_->retireLocked(impl_->watches.back());
    }
}

std::size_t FsWatcher::watchedCount() const {
    std::lock_guard<std::mutex> g(impl_->mu);
    return impl_->watches.size();
}

}  // namespace ftpflow
