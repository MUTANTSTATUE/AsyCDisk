#pragma once
#include <functional>
#include <memory>
#include <uv.h>
#include <vector>
class Session : public std::enable_shared_from_this<Session> {
public:
  using CloseCallback = std::function<void(std::shared_ptr<Session>)>;
  Session(uv_loop_t *loop);
  ~Session();
  // Prevent copy and assignment
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  uv_tcp_t *GetSocket() { return &socket_; }
  void Start();
  void Close();
  void SetCloseCallback(CloseCallback cb) { on_close_ = std::move(cb); }
  // Echo data back for now
  void Send(const char *data, size_t len);

private:
  struct WriteReq {
    uv_write_t req;
    uv_buf_t buf;
  };
  static void OnAlloc(uv_handle_t *handle, size_t suggested_size,
                      uv_buf_t *buf);
  static void OnRead(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
  static void OnWrite(uv_write_t *req, int status);
  static void OnClose(uv_handle_t *handle);
  uv_tcp_t socket_;
  CloseCallback on_close_;
  // Keeps the session alive during async ops
  std::shared_ptr<Session> self_ref_;
};
