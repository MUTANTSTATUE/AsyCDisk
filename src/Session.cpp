#include "Session.h"
#include "Logger.h"
#include "Database.h"
#include <cstring>

Session::Session(uv_loop_t *loop) {
  uv_tcp_init(loop, &socket_);
  socket_.data = this;
  fs_req_.data = this;
}

Session::~Session() {
  if (file_handle_ != -1) {
    uv_fs_t close_req;
    uv_fs_close(socket_.loop, &close_req, file_handle_, nullptr);
    uv_fs_req_cleanup(&close_req);
  }
  LOG_INFO("Session destroyed.");
}

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
  std::string password = req.json_payload.value("password", "");
  LOG_INFO("User login attempt: {}", username);
  
  if (Database::GetInstance().AuthenticateUser(username, password, user_id_)) {
      LOG_INFO("Login success for user: {} (ID: {})", username, user_id_);
      SendResponse(Protocol::Command::Login, 200,
                   {{"msg", "login success"}, {"user_id", user_id_}}, {});
  } else {
      LOG_WARN("Login failed for user: {}", username);
      SendResponse(Protocol::Command::Login, 401, {{"msg", "invalid username or password"}}, {});
  }
}

void Session::HandleListDir(const Protocol::Message &req) {
  if (user_id_ == -1) {
      SendResponse(Protocol::Command::ListDir, 403, {{"msg", "not logged in"}}, {});
      return;
  }

  int parent_id = req.json_payload.value("parent_id", 0);
  auto files = Database::GetInstance().ListFiles(user_id_, parent_id);
  SendResponse(Protocol::Command::ListDir, 200, {{"files", files}}, {});
}

void Session::HandleUploadReq(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::UploadReq, 403, {{"msg", "not logged in"}}, {});
    return;
  }

  std::string filename = req.json_payload.value("filename", "unknown");
  size_t total_size = req.json_payload.value("filesize", 0);
  LOG_INFO("Client wants to upload file: {}, total_size: {}", filename, total_size);

  std::string user_dir = "data/" + std::to_string(user_id_);
  std::string full_path = user_dir + "/" + filename;

  // 1. Ensure user directory exists
  uv_fs_mkdir(socket_.loop, &fs_req_, user_dir.c_str(), 0755, nullptr);
  uv_fs_req_cleanup(&fs_req_);

  // 2. Check if file already exists to support resumable upload
  uint64_t current_offset = 0;
  uv_fs_t stat_req;
  int r = uv_fs_stat(socket_.loop, &stat_req, full_path.c_str(), nullptr);
  if (r == 0) {
      current_offset = stat_req.statbuf.st_size;
      LOG_INFO("File exists, current size: {}", current_offset);
  }
  uv_fs_req_cleanup(&stat_req);

  if (current_offset >= total_size && total_size > 0) {
      // File already fully uploaded or larger?
      SendResponse(Protocol::Command::UploadReq, 200, {{"msg", "already uploaded"}, {"offset", current_offset}}, {});
      return;
  }

  // 3. Open file for writing (append-like mode: O_CREAT | O_WRONLY)
  // Note: We don't use O_TRUNC here so we can resume.
  r = uv_fs_open(socket_.loop, &fs_req_, full_path.c_str(),
                     O_WRONLY | O_CREAT, 0644, Session::OnFileOpen);
  if (r < 0) {
    LOG_ERROR("uv_fs_open error: {}", uv_strerror(r));
    SendResponse(Protocol::Command::UploadReq, 500, {{"msg", "io error"}}, {});
  } else {
      // OnFileOpen will handle the response, but we need to pass the offset.
      // We store the requested offset in the session.
      file_offset_ = current_offset; 
  }
}

void Session::HandleUploadData(const Protocol::Message &req) {
  if (file_handle_ == -1) {
    SendResponse(Protocol::Command::UploadData, 400, {{"msg", "file not open"}}, {});
    return;
  }

  if (req.header.binary_len == 0) {
      // EOF or empty chunk
      LOG_INFO("Upload finished for session.");
      uv_fs_close(socket_.loop, &fs_req_, file_handle_, Session::OnFileClose);
      return;
  }

  // We need to keep the data alive until uv_fs_write completes.
  // Since we are using a shared_ptr for the session and the req is local to HandleMessage,
  // we should copy the data.
  struct WriteCtx {
      Session* session;
      std::vector<char> data;
      uv_buf_t buf;
  };
  
  WriteCtx* ctx = new WriteCtx();
  ctx->session = this;
  ctx->data = req.binary_payload;
  ctx->buf = uv_buf_init(ctx->data.data(), ctx->data.size());

  // Using a separate request for write to avoid conflicting with the session's fs_req_ 
  // if multiple writes happen (though here they are sequential from client).
  // Actually, for simplicity and safety, we'll use a new uv_fs_t for each write.
  uv_fs_t* write_req = new uv_fs_t();
  write_req->data = ctx;

  int r = uv_fs_write(socket_.loop, write_req, file_handle_, &ctx->buf, 1, file_offset_, [](uv_fs_t* req){
      WriteCtx* ctx = static_cast<WriteCtx*>(req->data);
      if (req->result < 0) {
          LOG_ERROR("uv_fs_write error: {}", uv_strerror(req->result));
      } else {
          ctx->session->file_offset_ += req->result;
      }
      uv_fs_req_cleanup(req);
      delete req;
      delete ctx;
  });

  if (r < 0) {
      LOG_ERROR("uv_fs_write start error: {}", uv_strerror(r));
      delete write_req;
      delete ctx;
  }
}

void Session::HandleDownloadReq(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::DownloadData, 403, {{"msg", "not logged in"}}, {});
    return;
  }

  std::string filename = req.json_payload.value("filename", "");
  uint64_t offset = req.json_payload.value("offset", 0);
  if (filename.empty()) {
      SendResponse(Protocol::Command::DownloadData, 400, {{"msg", "missing filename"}}, {});
      return;
  }

  std::string full_path = "data/" + std::to_string(user_id_) + "/" + filename;
  file_offset_ = offset;
  
  // Open file for reading
  int r = uv_fs_open(socket_.loop, &fs_req_, full_path.c_str(), O_RDONLY, 0, [](uv_fs_t* req){
      Session* session = static_cast<Session*>(req->data);
      if (req->result >= 0) {
          session->file_handle_ = req->result;
          LOG_INFO("File opened for reading: {}, fd: {}, offset: {}", req->path, session->file_handle_, session->file_offset_);
          
          // Trigger first read
          uv_buf_t buf = uv_buf_init(session->file_read_buf_, sizeof(session->file_read_buf_));
          uv_fs_read(session->socket_.loop, req, session->file_handle_, &buf, 1, session->file_offset_, Session::OnFileRead);
      } else {
          LOG_ERROR("Failed to open file for download: {}", uv_strerror(req->result));
          session->SendResponse(Protocol::Command::DownloadData, 404, {{"msg", "file not found"}}, {});
          uv_fs_req_cleanup(req);
      }
  });
  
  if (r < 0) {
      LOG_ERROR("uv_fs_open start error: {}", uv_strerror(r));
      SendResponse(Protocol::Command::DownloadData, 500, {{"msg", "io error"}}, {});
  }
}

void Session::OnFileOpen(uv_fs_t *req) {
  Session *session = static_cast<Session *>(req->data);
  if (req->result >= 0) {
    session->file_handle_ = req->result;
    // session->file_offset_ is already set in HandleUploadReq or HandleDownloadReq
    LOG_INFO("File opened successfully, fd: {}, start offset: {}", session->file_handle_, session->file_offset_);
    session->SendResponse(Protocol::Command::UploadReq, 200, {{"msg", "ready"}, {"offset", session->file_offset_}}, {});
  } else {
    LOG_ERROR("OnFileOpen error: {}", uv_strerror(req->result));
    session->SendResponse(Protocol::Command::UploadReq, 500, {{"msg", "open failed"}}, {});
  }
  uv_fs_req_cleanup(req);
}

void Session::OnFileClose(uv_fs_t *req) {
  Session *session = static_cast<Session *>(req->data);
  LOG_INFO("File closed, fd: {}", session->file_handle_);
  session->file_handle_ = -1;
  session->SendResponse(Protocol::Command::UploadData, 200, {{"msg", "upload complete"}}, {});
  uv_fs_req_cleanup(req);
}

void Session::OnFileWrite(uv_fs_t *req) {
    // Handled in-line in HandleUploadData for now to simplify
}

void Session::OnFileRead(uv_fs_t *req) {
  Session *session = static_cast<Session *>(req->data);
  if (req->result > 0) {
      size_t bytes_read = req->result;
      session->file_offset_ += bytes_read;
      
      std::vector<char> data(session->file_read_buf_, session->file_read_buf_ + bytes_read);
      session->SendResponse(Protocol::Command::DownloadData, 200, {{"eof", false}}, data);
      
      // Continue reading next chunk
      uv_buf_t buf = uv_buf_init(session->file_read_buf_, sizeof(session->file_read_buf_));
      uv_fs_read(session->socket_.loop, req, session->file_handle_, &buf, 1, session->file_offset_, Session::OnFileRead);
  } else if (req->result == 0) {
      // EOF
      LOG_INFO("Download complete for session.");
      session->SendResponse(Protocol::Command::DownloadData, 200, {{"eof", true}}, {});
      uv_fs_close(session->socket_.loop, req, session->file_handle_, Session::OnFileClose);
  } else {
      LOG_ERROR("uv_fs_read error: {}", uv_strerror(req->result));
      session->SendResponse(Protocol::Command::DownloadData, 500, {{"msg", "read error"}}, {});
      uv_fs_close(session->socket_.loop, req, session->file_handle_, Session::OnFileClose);
  }
}

