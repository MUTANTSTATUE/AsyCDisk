#include "Session.h"
#include "Logger.h"
#include <cstring>

Session::Session(uv_loop_t* loop) {
    uv_tcp_init(loop, &socket_);
    socket_.data = this;
}

Session::~Session() {
    LOG_INFO("Session destroyed.");
}

void Session::Start() {
    self_ref_ = shared_from_this();
    uv_read_start((uv_stream_t*)&socket_, Session::OnAlloc, Session::OnRead);
    LOG_INFO("Session started reading.");
}

void Session::Close() {
    if (!uv_is_closing((uv_handle_t*)&socket_)) {
        uv_close((uv_handle_t*)&socket_, Session::OnClose);
    }
}

void Session::Send(const char* data, size_t len) {
    if (uv_is_closing((uv_handle_t*)&socket_)) return;

    WriteReq* wr = new WriteReq;
    wr->buf.base = new char[len];
    wr->buf.len = len;
    std::memcpy(wr->buf.base, data, len);

    int r = uv_write(&wr->req, (uv_stream_t*)&socket_, &wr->buf, 1, Session::OnWrite);
    if (r < 0) {
        LOG_ERROR("uv_write error: {}", uv_strerror(r));
        delete[] wr->buf.base;
        delete wr;
        Close();
    }
}

void Session::OnAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void Session::OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    Session* session = static_cast<Session*>(stream->data);

    if (nread > 0) {
        LOG_INFO("Session received {} bytes.", nread);
        // Echo test
        session->Send(buf->base, nread);
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            LOG_ERROR("Session read error: {}", uv_err_name(nread));
        } else {
            LOG_INFO("Session closed by client (EOF).");
        }
        session->Close();
    }

    if (buf->base) {
        delete[] buf->base;
    }
}

void Session::OnWrite(uv_write_t* req, int status) {
    if (status < 0) {
        LOG_ERROR("Write error: {}", uv_strerror(status));
    }
    
    WriteReq* wr = reinterpret_cast<WriteReq*>(req);
    delete[] wr->buf.base;
    delete wr;
}

void Session::OnClose(uv_handle_t* handle) {
    Session* session = static_cast<Session*>(handle->data);
    LOG_INFO("Session closed handle.");
    if (session->on_close_) {
        session->on_close_(session->self_ref_);
    }
    // Release self-reference, allowing the object to be destroyed
    session->self_ref_.reset();
}
