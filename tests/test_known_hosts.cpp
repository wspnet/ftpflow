// ftpflow tests -- known_hosts store + fingerprints.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ftpflow/known_hosts.hpp"
#include "ftpflow/sha256.hpp"

using namespace ftpflow;

static int g_checks = 0;
static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                 \
        }                                                               \
    } while (0)

// A fake 32-byte "public key blob" we can round-trip.
static std::string fakeBlob(char seed, std::size_t n = 32) {
    std::string b(n, '\0');
    for (std::size_t i = 0; i < n; ++i) b[i] = (char)(seed + (char)i);
    return b;
}

static std::string tmpPath(const char* name) {
    // Under the test working dir; cleaned up by caller.
    std::string dir = ".";
    return dir + "/" + name;
}

int main() {
    // ---- base64 RFC 4648 vectors ------------------------------------------
    CHECK(KnownHosts::base64Encode("", 0) == "");
    CHECK(KnownHosts::base64Encode("f", 1) == "Zg==");
    CHECK(KnownHosts::base64Encode("fo", 2) == "Zm8=");
    CHECK(KnownHosts::base64Encode("foo", 3) == "Zm9v");
    CHECK(KnownHosts::base64Encode("foob", 4) == "Zm9vYg==");
    CHECK(KnownHosts::base64Encode("fooba", 5) == "Zm9vYmE=");
    CHECK(KnownHosts::base64Encode("foobar", 6) == "Zm9vYmFy");

    {
        bool ok = true;
        CHECK(KnownHosts::base64Decode("Zg==", &ok) == "f" && ok);
        CHECK(KnownHosts::base64Decode("Zm8=", &ok) == "fo" && ok);
        CHECK(KnownHosts::base64Decode("Zm9v", &ok) == "foo" && ok);
        CHECK(KnownHosts::base64Decode("Zm9vYmFy", &ok) == "foobar" && ok);
    }
    {
        bool ok = true;
        KnownHosts::base64Decode("!!!!", &ok);
        CHECK(!ok);   // invalid alphabet
    }
    // Round-trip a binary blob (has arbitrary bytes, exercises padding).
    {
        const std::string blob = fakeBlob('K', 37);
        bool ok = false;
        const std::string enc = KnownHosts::base64Encode(blob.data(), blob.size());
        const std::string dec = KnownHosts::base64Decode(enc, &ok);
        CHECK(ok && dec == blob);
    }

    // ---- hexOf ------------------------------------------------------------
    {
        const unsigned char b[3] = {0x00, 0x0A, 0xFF};
        CHECK(KnownHosts::hexOf(b, 3) == "000aff");
    }

    // ---- fingerprint: SHA256:<base64(sha256(blob))> -----------------------
    {
        // Cross-check against the shipped Sha256 + a known OpenSSH vector:
        // the classic "server" example key is too long to inline here, so we
        // verify self-consistency: fingerprintOf(blob) == "SHA256:" +
        // base64(sha256(blob)).
        const std::string blob = fakeBlob('F');
        const std::vector<std::uint8_t> d =
            Sha256::digest(blob.data(), blob.size());
        const std::string expected = "SHA256:" +
            KnownHosts::base64Encode(d.data(), d.size());
        CHECK(KnownHosts::fingerprintOf(blob) == expected);
    }
    // Known real vector: SHA256 of the blob bytes {1,2,3} is fixed; check
    // the full OpenSSH fingerprint string shape.
    {
        const std::string fp = KnownHosts::fingerprintOf(std::string("\x01\x02\x03", 3));
        CHECK(fp.rfind("SHA256:", 0) == 0);   // starts with "SHA256:"
        CHECK(fp.size() > 8);
    }

    // ---- store: save/load round-trip --------------------------------------
    const std::string path = tmpPath("test_known_hosts.dat");
    std::remove(path.c_str());
    {
        KnownHosts kh(path);
        std::vector<KnownHostEntry> entries;
        KnownHostEntry a; a.host = "example.com"; a.port = 22;
        a.keyType = "ssh-rsa"; a.blob = fakeBlob('A');
        KnownHostEntry b; b.host = "192.168.1.10"; b.port = 2222;
        b.keyType = "ecdsa-sha2-nistp256"; b.blob = fakeBlob('B');
        entries.push_back(a);
        entries.push_back(b);

        std::string err;
        CHECK(kh.save(entries, &err));

        std::vector<KnownHostEntry> loaded;
        CHECK(kh.load(loaded, &err));
        CHECK(loaded.size() == 2);
        CHECK(loaded[0].host == "example.com" && loaded[0].port == 22);
        CHECK(loaded[0].keyType == "ssh-rsa" && loaded[0].blob == a.blob);
        CHECK(loaded[1].host == "192.168.1.10" && loaded[1].port == 2222);
        CHECK(loaded[1].keyType == "ecdsa-sha2-nistp256" &&
              loaded[1].blob == b.blob);
    }

    // ---- store: contains / trust / forget --------------------------------
    {
        KnownHosts kh(path);
        const std::string blob = fakeBlob('A');
        CHECK(kh.contains("example.com", 22, blob));
        CHECK(!kh.contains("example.com", 22, fakeBlob('Z')));
        CHECK(!kh.contains("other.com", 22, blob));
        CHECK(!kh.contains("example.com", 23, blob));   // different port

        std::string err;
        // trust replaces the existing example.com:22 entry.
        const std::string newBlob = fakeBlob('C');
        CHECK(kh.trust("example.com", 22, "ssh-rsa", newBlob, &err));
        CHECK(kh.contains("example.com", 22, newBlob));
        CHECK(!kh.contains("example.com", 22, fakeBlob('A')));

        // forget removes it.
        CHECK(kh.forget("example.com", 22, &err));
        CHECK(!kh.contains("example.com", 22, newBlob));
        // other entry survives.
        CHECK(kh.contains("192.168.1.10", 2222, fakeBlob('B')));
    }

    // ---- store: trustedHexFor returns blob hex ---------------------------
    {
        KnownHosts kh(path);
        const std::string blob = fakeBlob('B');
        const std::vector<std::string> hex =
            kh.trustedHexFor("192.168.1.10", 2222);
        CHECK(hex.size() == 1);
        CHECK(hex[0] == KnownHosts::hexOf(blob.data(), blob.size()));
        CHECK(kh.trustedHexFor("nope", 22).empty());
    }

    // ---- store: missing file is empty ------------------------------------
    {
        KnownHosts kh(tmpPath("test_known_hosts_missing.dat"));
        std::vector<KnownHostEntry> out;
        std::string err;
        CHECK(kh.load(out, &err));
        CHECK(out.empty());
    }

    // ---- store: malformed lines skipped, not fatal -----------------------
    {
        const std::string bad = tmpPath("test_known_hosts_bad.dat");
        std::remove(bad.c_str());
        {
            std::ofstream f(bad, std::ios::binary);
            f << "# a comment\n";
            f << "\n";
            f << "onefield\n";                 // 1 field, skipped
            f << "a b\n";                       // 2 fields, skipped
            f << "c d !!!\n";                   // 3 fields, bad base64, skipped
            f << "[good.example]:22 ssh-rsa "
              << KnownHosts::base64Encode(fakeBlob('G').data(), 32) << "\n";
            f << "badbase64.example ssh-rsa !!!!notbase64!!!!\n";
        }
        KnownHosts kh(bad);
        std::vector<KnownHostEntry> out;
        std::string err;
        CHECK(kh.load(out, &err));
        CHECK(out.size() == 1);
        CHECK(out[0].host == "good.example" && out[0].port == 22);
    }

    // ---- store: disabled (empty path) is a no-op -------------------------
    {
        KnownHosts kh("");
        std::vector<KnownHostEntry> out;
        std::string err;
        CHECK(kh.load(out, &err));   // trivially true
        CHECK(kh.trust("x", 22, "ssh-rsa", fakeBlob('D'), &err));
        CHECK(kh.trustedHexFor("x", 22).empty());
    }

    std::remove(path.c_str());
    std::remove("test_known_hosts_missing.dat");
    std::remove(tmpPath("test_known_hosts_bad.dat").c_str());

    std::printf("test_known_hosts: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
