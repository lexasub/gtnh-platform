#pragma once

#include "io_uring_context.h"
#include "io_uring_connection.h"

#include <cstdint>
#include <functional>
#include <string>

namespace gtnh::net {

class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer() { stop(); }

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    bool listen(uint16_t port, const char* name = "server");
    void stop();

    std::move_only_function<void(int client_fd)> on_accept;

    IoUringContext* context() { return &ctx_; }

private:
    void do_accept();

    IoUringContext ctx_;
    int listen_fd_ = -1;
    std::string name_;
    bool stopped_ = false;
};

} // namespace gtnh::net