#include "TcpServer.h"
#include "Logger.h"

TcpServer::TcpServer(EventLoop* loop, const std::string& ip, int port)
    : loop_(loop), ip_(ip), port_(port), is_running_(false) {
    
    uv_tcp_init(loop_->GetLoop(), &tcp_server_);
    tcp_server_.data = this;
}

TcpServer::~TcpServer() {
    Stop();
}

bool TcpServer::Start() {
    if (is_running_) return true;

    struct sockaddr_in addr;
    uv_ip4_addr(ip_.c_str(), port_, &addr);

    uv_tcp_bind(&tcp_server_, (const struct sockaddr*)&addr, 0);
    
    int r = uv_listen((uv_stream_t*)&tcp_server_, 128, TcpServer::OnConnection);
    if (r) {
        LOG_ERROR("Listen error: {}", uv_strerror(r));
        return false;
    }

    is_running_ = true;
    LOG_INFO("TcpServer listening on {}:{}", ip_, port_);
    return true;
}

void TcpServer::Stop() {
    if (is_running_) {
        if (!uv_is_closing((uv_handle_t*)&tcp_server_)) {
            uv_close((uv_handle_t*)&tcp_server_, nullptr);
        }
        is_running_ = false;
        LOG_INFO("TcpServer stopped.");
    }
}

void TcpServer::OnConnection(uv_stream_t* server, int status) {
    TcpServer* tcp_server = static_cast<TcpServer*>(server->data);
    if (tcp_server->on_new_connection_) {
        tcp_server->on_new_connection_(server, status);
    } else {
        LOG_WARN("New connection received, but no callback registered.");
    }
}
