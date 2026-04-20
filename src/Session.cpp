#include "Session.h"
#include "Logger.h"
#include <cstring>

Session::Session(uv_loop_t *loop) {
  uv_tcp_init(loop, &socket_);
  socket_.data = this;
}

Session::~Session() { LOG_INFO("Session destroyed."); }

void Session::Start() {
  self_ref_ = shared_from_this();
  uv_read_start((uv_stream_t *)&socket_, Session::OnAlloc, Session::OnRead);
  LOG_INFO("Session started reading.");
}

void Session::Close() {
  if (!uv_is_closing((uv_handle_t *)&socket_)) {
    uv_close((uv_handle_t *)&socket_, Session::OnClose);
  }
}

void Session::Send(const char *data, size_t len) {
  if (uv_is_closing((uv_handle_t *)&socket_))
    return;

  WriteReq *wr = new WriteReq;
  wr->buf.base = new char[len];
  wr->buf.len = len;
  std::memcpy(wr->buf.base, data, len);

  int r = uv_write(&wr->req, (uv_stream_t *)&socket_, &wr->buf, 1,
                   Session::OnWrite);
  if (r < 0) {
    LOG_ERROR("uv_write error: {}", uv_strerror(r));
    delete[] wr->buf.base;
    delete wr;
    Close();
  }
}

void Session::OnAlloc(uv_handle_t *handle, size_t suggested_size,
                      uv_buf_t *buf) {
  buf->base = new char[suggested_size];
  buf->len = suggested_size;
}

void Session::OnRead(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  Session *session = static_cast<Session *>(stream->data);

  if (nread > 0) {
    LOG_TRACE("Session received {} bytes.", nread);
    session->recv_buf_.insert(session->recv_buf_.end(), buf->base,
                              buf->base + nread);
    session->ProcessBuffer();
  } else if (nread < 0) {
    if (nread == UV_EOF) {
      LOG_INFO("Session closed by client (EOF).");
    } else if (nread == UV_ECONNRESET) {
      LOG_INFO("Session connection reset by client (ECONNRESET).");
    } else {
      LOG_ERROR("Session read error: {}", uv_err_name(nread));
    }
    session->Close();
  }

  if (buf->base) {
    delete[] buf->base;
  }
}

void Session::OnWrite(uv_write_t *req, int status) {
  if (status < 0) {
    LOG_ERROR("Write error: {}", uv_strerror(status));
  }

  WriteReq *wr = reinterpret_cast<WriteReq *>(req);
  delete[] wr->buf.base;
  delete wr;
}

void Session::OnClose(uv_handle_t *handle) {
  Session *session = static_cast<Session *>(handle->data);
  LOG_INFO("Session closed handle.");
  if (session->on_close_) {
    session->on_close_(session->self_ref_);
  }
  // Release self-reference, allowing the object to be destroyed
  session->self_ref_.reset();
}

void Session::ProcessBuffer() {
  while (recv_buf_.size() >= Protocol::HEADER_SIZE) {
    Protocol::Header header;
    std::memcpy(&header, recv_buf_.data(), Protocol::HEADER_SIZE);

    if (header.magic != Protocol::MAGIC_NUMBER) {
      LOG_ERROR("Invalid magic number 0x{:X}! Closing connection.",
                header.magic);
      Close();
      return;
    }

    size_t total_size =
        Protocol::HEADER_SIZE + header.json_len + header.binary_len;

    // Security limit (e.g. 50MB per message max in memory)
    if (total_size > 1024 * 1024 * 50) {
      LOG_ERROR("Message too large! Size: {}. Closing connection.", total_size);
      Close();
      return;
    }

    if (recv_buf_.size() >= total_size) {
      // We have a full packet
      Protocol::Message msg;
      msg.header = header;

      size_t offset = Protocol::HEADER_SIZE;
      if (header.json_len > 0) {
        std::string json_str(recv_buf_.data() + offset, header.json_len);
        try {
          msg.json_payload = nlohmann::json::parse(json_str);
        } catch (const std::exception &e) {
          LOG_ERROR("JSON parse error: {}", e.what());
          Close();
          return;
        }
        offset += header.json_len;
      }

      if (header.binary_len > 0) {
        msg.binary_payload.assign(recv_buf_.data() + offset,
                                  recv_buf_.data() + offset +
                                      header.binary_len);
        offset += header.binary_len;
      }

      // Remove parsed data from buffer
      recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + total_size);

      HandleMessage(msg);
    } else {
      // Not enough data yet
      break;
    }
  }
}

void Session::HandleMessage(const Protocol::Message &msg) {
  LOG_INFO("Received full message: CommandID={}, JSON='{}', BinarySize={}",
           static_cast<int>(msg.header.command), msg.json_payload.dump(),
           msg.binary_payload.size());

  switch (static_cast<Protocol::Command>(msg.header.command)) {
  case Protocol::Command::Ping:
    HandlePing(msg);
    break;
  case Protocol::Command::Login:
    HandleLogin(msg);
    break;
  case Protocol::Command::ListDir:
    HandleListDir(msg);
    break;
  case Protocol::Command::UploadReq:
    HandleUploadReq(msg);
    break;
  case Protocol::Command::UploadData:
    HandleUploadData(msg);
    break;
  case Protocol::Command::DownloadReq:
    HandleDownloadReq(msg);
    break;
  default:
    LOG_WARN("Unknown command ID: {}", static_cast<int>(msg.header.command));
    SendResponse(static_cast<Protocol::Command>(msg.header.command), 400,
                 {{"error", "Unknown command"}}, {});
    break;
  }
}

void Session::SendResponse(Protocol::Command cmd, uint16_t status,
                           const nlohmann::json &json_payload,
                           const std::vector<char> &binary_payload) {
  Protocol::Header header;
  header.magic = Protocol::MAGIC_NUMBER;
  header.version = Protocol::CURRENT_VERSION;
  header.command = static_cast<uint16_t>(cmd);
  header.status = status;

  std::string json_str;
  if (!json_payload.is_null() && !json_payload.empty()) {
    json_str = json_payload.dump();
  }
  header.json_len = json_str.size();
  header.binary_len = binary_payload.size();

  std::vector<char> response(Protocol::HEADER_SIZE + header.json_len +
                             header.binary_len);
  std::memcpy(response.data(), &header, Protocol::HEADER_SIZE);

  size_t offset = Protocol::HEADER_SIZE;
  if (header.json_len > 0) {
    std::memcpy(response.data() + offset, json_str.c_str(), header.json_len);
    offset += header.json_len;
  }
  if (header.binary_len > 0) {
    std::memcpy(response.data() + offset, binary_payload.data(),
                header.binary_len);
  }

  Send(response.data(), response.size());
}

void Session::HandlePing(const Protocol::Message &req) {
  SendResponse(Protocol::Command::Ping, 200, {{"msg", "pong"}}, {});
}

void Session::HandleLogin(const Protocol::Message &req) {
  std::string username = req.json_payload.value("username", "");
  LOG_INFO("User login attempt: {}", username);
  // TODO: Verify with SQLite
  SendResponse(Protocol::Command::Login, 200,
               {{"msg", "login success", "token", "dummy_token"}}, {});
}

void Session::HandleListDir(const Protocol::Message &req) {
  // TODO: Fetch directory list from SQLite
  SendResponse(Protocol::Command::ListDir, 200,
               {{"files", nlohmann::json::array()}}, {});
}

void Session::HandleUploadReq(const Protocol::Message &req) {
  std::string filename = req.json_payload.value("filename", "unknown");
  size_t filesize = req.json_payload.value("filesize", 0);
  LOG_INFO("Client wants to upload file: {}, size: {}", filename, filesize);
  // TODO: Prepare file stream (uv_fs_open)
  SendResponse(Protocol::Command::UploadReq, 200, {{"msg", "ready for data"}},
               {});
}

void Session::HandleUploadData(const Protocol::Message &req) {
  LOG_INFO("Received {} bytes of file data.", req.header.binary_len);
  // TODO: Write to uv_fs
  SendResponse(Protocol::Command::UploadData, 200, {{"msg", "chunk received"}},
               {});
}

void Session::HandleDownloadReq(const Protocol::Message &req) {
  std::string filename = req.json_payload.value("filename", "unknown");
  LOG_INFO("Client requested download for: {}", filename);

  // TODO: Read from uv_fs. For now, simulate sending back dummy binary data.
  std::vector<char> dummy_data(10, 'A');
  SendResponse(Protocol::Command::DownloadData, 200,
               {{"filename", filename}, {"eof", true}}, dummy_data);
}
