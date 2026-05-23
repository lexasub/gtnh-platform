#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <string>

namespace gtnh::net {

// Blocking TCP connector with timeout.
// Used at service startup — ok to block briefly.
struct TcpConnector {
    // Blocking TCP connect with timeout.
    // Returns fd on success (SOCK_NONBLOCK + TCP_NODELAY set), -1 on failure.
    static int connect(const char* host, uint16_t port, int timeout_ms = 5000);

    // DNS resolve only, returns false on failure.
    static bool resolve(const char* host, sockaddr_in& out);
};

} // namespace gtnh::net
