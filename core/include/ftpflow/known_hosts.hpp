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
    // false (and fills `error`) only on I/O or structural corruption; malformed
    // individual lines are skipped, not fatal.
    bool load(std::vector<KnownHostEntry>& out, std::string* error = nullptr) const;

    // Atomically write all entries (tmp + rename). Creates the parent dir.
    bool save(const std::vector<KnownHostEntry>& entries,
              std::string* error = nullptr) const;

    // True if `host:port` already trusts exactly `blob` (binary compare).
    bool contains(const std::string& host, std::uint16_t port,
                  const std::string& blob) const;

    // Add `host:port -> blob`, replacing any prior blob for that host:port.
    // Load-modify-save; returns false on I/O error.
    bool trust(const std::string& host, std::uint16_t port,
               const std::string& keyType, const std::string& blob,
               std::string* error = nullptr) const;

    // Drop every entry for `host:port`. Returns false on I/O error.
    bool forget(const std::string& host, std::uint16_t port,
                std::string* error = nullptr) const;

    // All trusted blobs for `host:port`, as lowercase hex — ready to feed
    // straight into SshExec/SftpSession's `trustedHostKeyHex` parameter.
    std::vector<std::string> trustedHexFor(const std::string& host,
                                           std::uint16_t port) const;

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
