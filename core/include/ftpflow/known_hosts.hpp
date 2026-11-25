// ftpflow -- OpenSSH-compatible known_hosts store + host-key fingerprints.
// Part of core.lib: pure C++17, no Qt, no libssh2 dependency.
//
// The on-disk format matches OpenSSH `known_hosts`: one entry per line,
// `<hostport> <keytype> <base64 blob> [comment]`, e.g.
//
//     [github.com]:22 ssh-rsa AAAAB3NzaC1yc2EAAA...
//     192.168.1.10 ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTIt...
//
// This lets the store interoperate with the system `ssh` client while we
// verify the exact public-key blob (matching SshExec/SftpSession's existing
// trustedHostKeyHex contract). The keytype string is caller-supplied (the
// SSH layer derives it from libssh2's hostkey-type enum); core only stores
// and hashes the raw blob.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ftpflow {

// One normalized known-host entry.
struct KnownHostEntry {
    std::string host;    // hostname or IP, no port
    std::uint16_t port = 22;
    std::string keyType;   // "ssh-rsa", "ecdsa-sha2-nistp256", ...
    std::string blob;      // raw public-key blob (binary)
    // OpenSSH `@revoked` marker: the blob must NEVER be trusted again.
    // Revoked entries survive load/save round-trips and block trust().
    bool revoked = false;
};

class KnownHosts {
public:
    explicit KnownHosts(std::string path);
    ~KnownHosts() = default;
    KnownHosts(const KnownHosts&) = delete;
    KnownHosts& operator=(const KnownHosts&) = delete;

    // %APPDATA%/ftpflow/known_hosts
    static std::string defaultPath();

    bool enabled() const { return !path_.empty(); }

    // Load all entries. Missing file == empty store (returns true). Returns
    // false (and fills `error`) when the file EXISTS but cannot be read --
    // that must never be silently downgraded to "empty store", or a locked/
    // ACL'd file would bypass pinning (trust-on-first-use downgrade).
    // Malformed individual lines are skipped, not fatal. `@revoked` lines
    // load with revoked=true; `@cert-authority` / hashed-host lines are
    // skipped (unsupported).
    bool load(std::vector<KnownHostEntry>& out, std::string* error = nullptr) const;

    // Atomically write all entries (tmp + rename). Creates the parent dir.
    bool save(const std::vector<KnownHostEntry>& entries,
              std::string* error = nullptr) const;

    // True if `host:port` already trusts exactly `blob` (binary compare).
    bool contains(const std::string& host, std::uint16_t port,
                  const std::string& blob) const;

    // Add `host:port -> blob`, replacing any prior non-revoked blob for that
    // host:port. Load-modify-save; returns false on I/O error. Refuses when
    // the exact blob is listed as `@revoked` (a revoked key must stay dead),
    // and never replaces or drops revoked entries.
    bool trust(const std::string& host, std::uint16_t port,
               const std::string& keyType, const std::string& blob,
               std::string* error = nullptr) const;

    // Drop every NON-revoked entry for `host:port` (revoked keys stay so
    // they cannot be re-accepted). Returns false on I/O error.
    bool forget(const std::string& host, std::uint16_t port,
                std::string* error = nullptr) const;

    // True when `blob` is listed as `@revoked` for `host:port`.
    bool isRevoked(const std::string& host, std::uint16_t port,
                   const std::string& blob) const;

    // All trusted blobs for `host:port`, as lowercase hex — ready to feed
    // straight into SshExec/SftpSession's `trustedHostKeyHex` parameter.
    // Revoked entries are excluded. When `storeOk` is given it is set to
    // false if the store existed but could not be read (callers must then
    // FAIL the connection instead of falling through to trust-on-first-use).
    std::vector<std::string> trustedHexFor(const std::string& host,
                                           std::uint16_t port,
                                           bool* storeOk = nullptr) const;

    // ---- fingerprints -----------------------------------------------------
    // OpenSSH-style host-key fingerprint: "SHA256:<base64(sha256(blob))>".
    // Uses the same SHA-256 the rest of core ships; no extra crypto dep.
    static std::string fingerprintOf(const std::string& blob);

    // Base64 encode/decode (RFC 4648, standard alphabet, no line wrapping).
    static std::string base64Encode(const void* data, std::size_t len);
    static std::string base64Decode(const std::string& s, bool* ok = nullptr);

    // Hex encode (lowercase).
    static std::string hexOf(const void* data, std::size_t len);
    // Hex decode; returns empty and sets *ok=false on a non-hex byte or odd
    // length.
    static std::string hexDecode(const std::string& s, bool* ok = nullptr);

    // Fingerprint of a hex-encoded blob (matches the observedHostKeyHex
    // returned by SshExec/SftpSession on HostKeyRejected).
    static std::string fingerprintOfHex(const std::string& hex);

private:
    std::string path_;
};

}  // namespace ftpflow
