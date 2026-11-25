// ftpflow -- free-form "site card" text recognition (connect dialog).
//
// Pure C++ (no Qt) so the parser is unit-testable. Input is UTF-8 text like
//   "@小号 www.mailvip163.com 116.196.115.111 FTP账号：zmailvip163com8 FTP密码：MQ70v]y6lD"
#pragma once

#include <string>

namespace ftpflow {

// A recognized field set. Empty string / protocol == -1 means "not found".
struct SiteCard {
    std::string name;      // labeled 名称, else domain with leading "www." stripped
    std::string host;      // labeled 主机, else bare IPv4, else the domain as written
    std::string port;      // labeled 端口 or host:port token
    std::string user;      // labeled 账号/用户名/...
    std::string password;  // labeled 密码/口令/...
    int protocol = -1;     // -1 unknown, 0 = FTP, 1 = SFTP

    bool hasName() const { return !name.empty(); }
    bool hasHost() const { return !host.empty(); }
    bool hasPort() const { return !port.empty(); }
    bool hasUser() const { return !user.empty(); }
    bool hasPassword() const { return !password.empty(); }
    bool hasProtocol() const { return protocol >= 0; }
    bool any() const {
        return hasName() || hasHost() || hasPort() || hasUser() ||
               hasPassword() || hasProtocol();
    }
};

// Scan `text` (any mix of labels and bare tokens; multi-line is fine) and
// return everything that could be recognized. First match wins per field.
SiteCard parseSiteCard(const std::string& text);

}  // namespace ftpflow
