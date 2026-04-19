#pragma once

#include "EventLoop.h"
#include <string>
#include <uv.h>
#include <functional>

class TcpServer {
public:
    using NewConnectionCallback = std::function<void(uv_stream_t* server, int status)>;

    TcpServer(EventLoop* loop, const std::string& ip, int port);
    ~TcpServer();

    // Prevent copy and assignment
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool Start();
    void Stop();

    void SetNewConnectionCallback(NewConnectionCallback cb) { on_new_connection_ = std::move(cb); }
    
    EventLoop* GetLoop() const { return loop_; }

private:
    static void OnConnection(uv_stream_t* server, int status);

    EventLoop* loop_;
    std::string ip_;
    int port_;
    uv_tcp_t tcp_server_;
    bool is_running_;

    NewConnectionCallback on_new_connection_;
};
