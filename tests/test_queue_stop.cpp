// ftpflow -- emergency-stop regression (offline).
//
// Verifies that one cancelAll() drops a large queued batch in O(n), removes
// it from the durable journal, and leaves the queue usable for new jobs.
#include "ftpflow/transfer_queue.hpp"
#include "ftpflow/transfer_journal.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

static int g_checks = 0;
static int g_failed = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            ++g_failed;                                                      \
        }                                                                    \
    } while (0)

int main() {
    const std::string journal =
        "test_queue_stop_" + std::to_string(GetCurrentProcessId()) + ".dat";
    ::DeleteFileA(journal.c_str());

    ftpflow::TransferQueue q(2, 50);
    q.setJournal(journal);
    q.pauseAll();   // no network: jobs stay queued while we inspect state

    ftpflow::TransferRequest req;
    req.protocol = ftpflow::BackendKind::Ftp;
    req.direction = ftpflow::TransferDirection::Upload;
    req.localPath = "C:/ftpflow-stop-test/missing.bin";
    req.remotePath = "/missing.bin";
    req.siteName = "stop-test";

    for (int i = 0; i < 500; ++i) q.enqueue(req);
    CHECK(q.pending() == 500);
    CHECK(q.pendingJournalCount() == 500);

    const std::size_t stopped = q.cancelAll();
    CHECK(stopped == 500);
    CHECK(q.pending() == 0);
    CHECK(q.pendingJournalCount() == 0);   // restart must NOT restore them

    // The queue is still usable after the brake. Pause again so the dummy
    // job (missing local file) cannot fail and retire before we inspect it.
    q.pauseAll();   // keep the dummy job from failing before we inspect it
    q.enqueue(req);
    CHECK(q.pendingJournalCount() == 1);
    CHECK(q.cancelAll() == 1);
    CHECK(q.pendingJournalCount() == 0);

    q.shutdown();
    ::DeleteFileA(journal.c_str());

    // Regression: a failed row stays visible in the GUI long after the
    // 256-entry doneCache_ window has rolled over. retry() must still find
    // the original request for that old failure.
    {
        ftpflow::TransferQueue q2(4, 20);
        ftpflow::TransferRequest bad;
        bad.protocol = ftpflow::BackendKind::Ftp;
        bad.direction = ftpflow::TransferDirection::Upload;
        bad.localPath = "C:/ftpflow-retry-test/missing.bin";
        bad.remotePath = "/missing.bin";
        bad.siteName = "retry-test";
        bad.ftp.host = "127.0.0.1";
        bad.ftp.port = 1;
        bad.ftp.connectTimeoutMs = 200;

        const std::uint64_t first = q2.enqueue(bad);
        CHECK(first != 0);
        for (int i = 0; i < 300; ++i) q2.enqueue(bad);
        q2.waitAll();
        CHECK(q2.pending() == 0);
        // 301 terminal jobs >> kDoneCache(256): without the failed cache
        // this retry silently returned 0.
        CHECK(q2.retry(first, ftpflow::ConflictPolicy::Overwrite) != 0);
        q2.cancelAll();
        q2.shutdown();
    }

    std::printf("test_queue_stop: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
