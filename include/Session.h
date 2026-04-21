#pragma once
#include "Protocol.h"
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
  // Send data helper
  void Send(const char *data, size_t len);

private:
  void ProcessBuffer();
  void HandleMessage(const Protocol::Message &msg);

  void SendResponse(Protocol::Command cmd, uint16_t status,
                    const nlohmann::json &json_payload,
                    const std::vector<char> &binary_payload);

  void HandlePing(const Protocol::Message &req);
  void HandleLogin(const Protocol::Message &req);
  void HandleRegister(const Protocol::Message &req);
  void HandleListDir(const Protocol::Message &req);
  void HandleRemove(const Protocol::Message &req);
  void HandleUploadReq(const Protocol::Message &req);
  void HandleUploadData(const Protocol::Message &req);
  void HandleDownloadReq(const Protocol::Message &req);
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
  std::vector<char> recv_buf_;
  int user_id_ = -1;

  // File IO
  uint32_t pending_fs_reqs_ = 0;
  bool closing_pending_ = false;
  uv_file file_handle_ = -1;
  uint64_t file_offset_ = 0;
  uint64_t total_filesize_ = 0;
  std::string current_filename_;
  bool is_uploading_ = false;
  uv_fs_t fs_req_; // Reuse for sequential FS ops in this session

  static void OnFileOpen(uv_fs_t *req);
  static void OnFileWrite(uv_fs_t *req);
  static void OnFileRead(uv_fs_t *req);
  static void OnFileClose(uv_fs_t *req);

  char file_read_buf_[16384]; // 16KB read buffer
};
