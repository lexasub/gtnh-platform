#include <gtnh/net/server.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace gtnh::net {

bool TcpServer::listen(uint16_t port, const char* name) {
    name_ = name;

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        spdlog::error("{}: socket() failed: {}", name_, std::strerror(errno));
        return false;
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("{}: bind({}) failed: {}", name_, port, std::strerror(errno));
        ::close(fd);
        return false;
    }

    if (::listen(fd, 128) < 0) {
        spdlog::error("{}: listen() failed: {}", name_, std::strerror(errno));
        ::close(fd);
        return false;
    }

    listen_fd_ = fd;
    stopped_ = false;

    if (!ctx_.init()) {
        spdlog::error("{}: IoUringContext init failed", name_);
        ::close(fd);
        listen_fd_ = -1;
        return false;
    }

    ctx_.on_cqe = [this](int res, uint64_t user_data) {
        if (user_data == 0) {
            if (res < 0) {
                if (res != -ECANCELED) {
                    spdlog::error("{}: accept error: {}", name_, std::strerror(-res));
                }
                return;
            }
            int flags = fcntl(res, F_GETFL, 0);
            if (flags >= 0) fcntl(res, F_SETFL, flags | O_NONBLOCK);
            int opt = 1;
            setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            int keepalive = 1;
            setsockopt(res, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
            int keepidle = 5;
            setsockopt(res, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
            int keepintvl = 3;
            setsockopt(res, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
            int keepcnt = 3;
            setsockopt(res, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
            if (on_accept) {
                on_accept(res);
            }
            do_accept();
        }
    };

    do_accept();
    spdlog::info("{}: listening on port {}", name_, port);
    return true;
}

void TcpServer::stop() {
    stopped_ = true;
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    ctx_.shutdown();
}

void TcpServer::do_accept() {
    if (stopped_) return;
    io_uring_sqe* sqe = ctx_.get_sqe();
    if (!sqe) {
        spdlog::warn("{}: SQ full (accept)", name_);
        return;
    }
    io_uring_prep_accept(sqe, listen_fd_, nullptr, nullptr, 0);
    sqe->user_data = 0;
    ctx_.submit();
}

} // namespace gtnh::net