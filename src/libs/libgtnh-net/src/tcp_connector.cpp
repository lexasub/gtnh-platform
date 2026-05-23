#include <gtnh/net/tcp_connector.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gtnh::net {

int TcpConnector::connect(const char* host, uint16_t port, int timeout_ms) {
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* ai = nullptr;
    int rc = ::getaddrinfo(host, nullptr, &hints, &ai);
    if (rc != 0 || !ai) {
        spdlog::error("TcpConnector: getaddrinfo({}) failed: {}", host, gai_strerror(rc));
        return -1;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        spdlog::error("TcpConnector: socket() failed: {}", std::strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }

    int flag = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr   = reinterpret_cast<struct sockaddr_in*>(ai->ai_addr)->sin_addr;
    freeaddrinfo(ai);

    rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        spdlog::error("TcpConnector: connect() failed: {}", std::strerror(errno));
        ::close(fd);
        return -1;
    }

    // Blocking wait for connection
    struct pollfd pfd{fd, POLLOUT, 0};
    int poll_rc = ::poll(&pfd, 1, timeout_ms);
    if (poll_rc <= 0 || !(pfd.revents & POLLOUT)) {
        spdlog::error("TcpConnector: connect to {}:{} timed out ({}ms)", host, port, timeout_ms);
        ::close(fd);
        return -1;
    }

    int so_err = 0;
    socklen_t errlen = sizeof(so_err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &errlen);
    if (so_err != 0) {
        spdlog::error("TcpConnector: connect SO_ERROR: {}", std::strerror(so_err));
        ::close(fd);
        return -1;
    }

    return fd;
}

bool TcpConnector::resolve(const char* host, sockaddr_in& out) {
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* ai = nullptr;
    int rc = ::getaddrinfo(host, nullptr, &hints, &ai);
    if (rc != 0 || !ai) {
        spdlog::error("TcpConnector: resolve({}) failed: {}", host, gai_strerror(rc));
        return false;
    }

    out = *reinterpret_cast<struct sockaddr_in*>(ai->ai_addr);
    freeaddrinfo(ai);
    return true;
}

} // namespace gtnh::net
