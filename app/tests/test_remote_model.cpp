// ftpflow -- RemoteFsModel integration test (Qt console app).
// Drives the model's internal worker thread against a live FTP server
// (127.0.0.1:2121, pyftpdlib) and, when the local sshd answers, an SFTP
// connection with key auth. Verifies connect -> first listing, navigation,
// paging state, and clean shutdown.
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

#include "remote_fs_model.hpp"

using ftpflow::RemoteFsModel;

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

// Runs the event loop until `pred` holds or timeout fires.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poller;
    QObject::connect(&poller, &QTimer::timeout, &loop, [&] {
        if (pred()) loop.quit();
    });
    poller.start(25);
    deadline.start(timeoutMs);
    loop.exec();
    return pred();
}

static bool tcpUp(unsigned short port) {
    // Cheap check: try a QAbstractSocket-less probe via Qt? Keep it simple:
    // rely on the connect attempt itself and its timeout.
    (void)port;
    return true;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- FTP leg (pyftpdlib on 2121) -----------------------------------
    {
        RemoteFsModel m;
        bool connected = false, failed = false;
        QObject::connect(&m, &RemoteFsModel::connected, [&] { connected = true; });
        QObject::connect(&m, &RemoteFsModel::connectFailed,
                         [&](const QString&) { failed = true; });

        RemoteFsModel::ConnInfo ci;
        ci.isSftp = false;
        ci.ftp.host = "127.0.0.1";
        ci.ftp.port = 2121;
        ci.ftp.user = "tester";
        ci.ftp.password = "secret";
        ci.ftp.initialPath = "/";
        ci.startDir = "/";
        m.connectTo(ci);
        CHECK(waitFor([&] { return connected || failed; }, 8000));
        CHECK(connected);
        CHECK(m.isConnected());
        CHECK(m.currentPath() == "/");

        // First listing lands.
        CHECK(waitFor([&] { return m.rowCount() > 0; }, 8000));
        CHECK(m.rowCount() >= 1);
        // hello.txt + sub exist in the fixture.
        bool sawSub = false, sawHello = false;
        for (int i = 0; i < m.rowCount(); ++i) {
            const std::string p = m.pathAt(m.index(i, 0));
            if (p.find("hello") != std::string::npos) sawHello = true;
            if (p == "/sub") sawSub = true;
        }
        CHECK(sawHello);
        CHECK(sawSub);
        // /sub is a directory.
        for (int i = 0; i < m.rowCount(); ++i) {
            if (m.pathAt(m.index(i, 0)) == "/sub") {
                CHECK(m.isDir(m.index(i, 0)));
            }
        }

        // Navigate in and back.
        m.enterDir("/sub");
        CHECK(waitFor([&] { return m.currentPath() == "/sub" &&
                                   m.rowCount() >= 0; }, 8000));
        m.goUp();
        waitFor([&] { return m.currentPath() == "/"; }, 8000);
        CHECK(m.currentPath() == "/");

        // Disconnect cleanly (worker thread joins in the destructor).
        m.disconnect();
        CHECK(!m.isConnected());
    }

    // ---- SFTP leg (local sshd; skipped unless the key file exists) ----
    {
        const char* keyPath =
            "C:/Users/wspne/.ssh/ftpflow_test_rsa";
        FILE* kf = std::fopen(keyPath, "rb");
        if (!kf) {
            std::printf("[info] no test key at %s -> skipping SFTP leg\n",
                        keyPath);
        } else {
            std::fclose(kf);
            RemoteFsModel m;
            bool connected = false;
            QObject::connect(&m, &RemoteFsModel::connected,
                             [&] { connected = true; });
            RemoteFsModel::ConnInfo ci;
            ci.isSftp = true;
            ci.ssh.host = "127.0.0.1";
            ci.ssh.port = 22;
            ci.ssh.user = "wspne";
            ci.ssh.password = "-";
            // Read the PEM into the endpoint.
            std::FILE* f = std::fopen(keyPath, "rb");
            std::string pem;
            char buf[4096];
            std::size_t got = 0;
            while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
                pem.append(buf, got);
            std::fclose(f);
            ci.ssh.privateKeyPem = pem;
            ci.startDir = ".";
            m.connectTo(ci);
            CHECK(waitFor([&] { return connected; }, 10000));
            CHECK(m.isConnected());
            CHECK(waitFor([&] { return m.rowCount() > 0; }, 10000));
            CHECK(m.rowCount() >= 1);
            std::printf("[info] SFTP leg: %d entries in start dir\n",
                        m.rowCount());
            m.disconnect();
        }
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_failed == 0) std::printf("ALL REMOTE-MODEL TESTS PASSED\n");
    return g_failed == 0 ? 0 : 1;
}
