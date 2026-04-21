#include "Session.h"
#include "Database.h"
#include "Logger.h"
#include <cstring>
#include <uv.h>

struct IOCtx {
  std::shared_ptr<Session> session;
  uint32_t stream_id;
};

Session::Session(uv_loop_t *loop) {
  uv_tcp_init(loop, &socket_);
  socket_.data = this;
}

Session::~Session() {
  // Active tasks should ideally be closed, but for now just cleanup
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
  case Protocol::Command::Register:
    HandleRegister(msg);
    break;
  case Protocol::Command::ListDir:
    HandleListDir(msg);
    break;
  case Protocol::Command::Remove:
    HandleRemove(msg);
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
                 msg.header.stream_id,
                 {{"error", "Unknown command"}}, {});
    break;
  }
}

void Session::SendResponse(Protocol::Command cmd, uint16_t status,
                           uint32_t stream_id,
                           const nlohmann::json &json_payload,
                           const std::vector<char> &binary_payload) {
  Protocol::Header header;
  header.magic = Protocol::MAGIC_NUMBER;
  header.version = Protocol::CURRENT_VERSION;
  header.command = static_cast<uint16_t>(cmd);
  header.status = status;
  header.stream_id = stream_id;

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
  SendResponse(Protocol::Command::Ping, 200, req.header.stream_id,
               {{"msg", "pong"}}, {});
}

void Session::HandleLogin(const Protocol::Message &req) {
  std::string username = req.json_payload.value("username", "");
  std::string password = req.json_payload.value("password", "");
  LOG_INFO("User login attempt: {}", username);

  if (Database::GetInstance().AuthenticateUser(username, password, user_id_)) {
    LOG_INFO("Login success for user: {} (ID: {})", username, user_id_);
    SendResponse(Protocol::Command::Login, 200, req.header.stream_id,
                 {{"msg", "login success"}, {"user_id", user_id_}}, {});
  } else {
    LOG_WARN("Login failed for user: {}", username);
    SendResponse(Protocol::Command::Login, 401, req.header.stream_id,
                 {{"msg", "invalid username or password"}}, {});
  }
}

void Session::HandleRegister(const Protocol::Message &req) {
  std::string username = req.json_payload.value("username", "");
  std::string password = req.json_payload.value("password", "");

  if (username.empty() || password.empty()) {
    SendResponse(Protocol::Command::Register, 400, req.header.stream_id,
                 {{"msg", "username or password empty"}}, {});
    return;
  }

  if (Database::GetInstance().RegisterUser(username, password)) {
    LOG_INFO("New user registered: {}", username);
    SendResponse(Protocol::Command::Register, 200, req.header.stream_id,
                 {{"msg", "register success"}}, {});
  } else {
    LOG_WARN("Registration failed (user might exist): {}", username);
    SendResponse(Protocol::Command::Register, 409, req.header.stream_id,
                 {{"msg", "registration failed or user already exists"}}, {});
  }
}

void Session::HandleListDir(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::ListDir, 403, req.header.stream_id, {{"msg", "not logged in"}},
                 {});
    return;
  }

  int parent_id = req.json_payload.value("parent_id", 0);
  auto files = Database::GetInstance().ListFiles(user_id_, parent_id);
  SendResponse(Protocol::Command::ListDir, 200, req.header.stream_id, {{"files", files}}, {});
}

void Session::HandleRemove(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::Remove, 403, req.header.stream_id, {{"msg", "not logged in"}},
                 {});
    return;
  }

  int file_id = req.json_payload.value("file_id", -1);
  int parent_id = req.json_payload.value("parent_id", 0);

  if (file_id == -1) {
    SendResponse(Protocol::Command::Remove, 400, req.header.stream_id,
                 {{"msg", "missing file_id"}}, {});
    return;
  }

  auto file_info = Database::GetInstance().GetFileById(user_id_, file_id);
  if (file_info.empty()) {
    SendResponse(Protocol::Command::Remove, 404, req.header.stream_id,
                 {{"msg", "file id not found"}}, {});
    return;
  }

  std::string filename = file_info["filename"];

  // 1. Delete from database
  if (!Database::GetInstance().DeleteFile(user_id_, parent_id, filename)) {
    SendResponse(Protocol::Command::Remove, 500, req.header.stream_id,
                 {{"msg", "db error"}}, {});
    return;
  }

  // 2. Delete from filesystem (Async)
  std::string full_path = "data/" + std::to_string(user_id_) + "/" + filename;

  // Note: We use a new uv_fs_t for unlink to avoid conflict with other active
  // FS ops if any
  uv_fs_t *unlink_req = new uv_fs_t();
  unlink_req->data = new IOCtx{shared_from_this(), req.header.stream_id};

  int r = uv_fs_unlink(
      socket_.loop, unlink_req, full_path.c_str(), [](uv_fs_t *req) {
        auto *ctx = static_cast<IOCtx *>(req->data);
        if (req->result < 0 && req->result != UV_ENOENT) {
          LOG_ERROR("uv_fs_unlink error: {}", uv_strerror(req->result));
          ctx->session->SendResponse(Protocol::Command::Remove, 500,
                                     ctx->stream_id, {{"msg", "filesystem error"}},
                                     {});
        } else {
          LOG_INFO("File deleted: {}", req->path);
          ctx->session->SendResponse(Protocol::Command::Remove, 200,
                                     ctx->stream_id, {{"msg", "success"}}, {});
        }
        uv_fs_req_cleanup(req);
        delete ctx;
        delete req;
      });

  if (r < 0) {
    LOG_ERROR("uv_fs_unlink start error: {}", uv_strerror(r));
    delete static_cast<IOCtx *>(unlink_req->data);
    delete unlink_req;
    SendResponse(Protocol::Command::Remove, 500, req.header.stream_id,
                 {{"msg", "unlink failed to start"}}, {});
  }
}

void Session::HandleUploadReq(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::UploadReq, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  std::string filename = req.json_payload.value("filename", "unknown");
  size_t total_size = req.json_payload.value("filesize", 0);
  uint32_t stream_id = req.header.stream_id;

  LOG_INFO("Client wants to upload file: {}, total_size: {}, stream: {}",
           filename, total_size, stream_id);

  auto task = std::make_shared<FileTask>();
  task->stream_id = stream_id;
  task->current_filename = filename;
  task->total_filesize = total_size;
  task->is_uploading = true;
  active_tasks_[stream_id] = task;

  std::string user_dir = "data/" + std::to_string(user_id_);
  std::string full_path = user_dir + "/" + filename;

  // 1. Ensure user directory exists
  uv_fs_t mkdir_req;
  uv_fs_mkdir(socket_.loop, &mkdir_req, user_dir.c_str(), 0755, nullptr);
  uv_fs_req_cleanup(&mkdir_req);

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
    auto existing = Database::GetInstance().GetFile(user_id_, 0, filename);
    if (existing.empty() || existing["filesize"] != (int64_t)current_offset) {
      Database::GetInstance().AddFile(user_id_, 0, filename, current_offset,
                                      false, full_path);
    }
    SendResponse(Protocol::Command::UploadReq, 200, stream_id,
                 {{"msg", "already uploaded"}, {"offset", current_offset}}, {});
    active_tasks_.erase(stream_id);
    return;
  }

  // 3. Open file for writing
  task->file_offset = current_offset;
  uv_fs_t *open_req = new uv_fs_t();
  open_req->data = new IOCtx{shared_from_this(), stream_id};

  r = uv_fs_open(socket_.loop, open_req, full_path.c_str(), O_WRONLY | O_CREAT,
                 0644, Session::OnFileOpen);
  if (r < 0) {
    LOG_ERROR("uv_fs_open error: {}", uv_strerror(r));
    delete static_cast<IOCtx *>(open_req->data);
    delete open_req;
    SendResponse(Protocol::Command::UploadReq, 500, stream_id,
                 {{"msg", "io error"}}, {});
    active_tasks_.erase(stream_id);
  }
}

void Session::HandleUploadData(const Protocol::Message &req) {
  uint32_t stream_id = req.header.stream_id;
  auto it = active_tasks_.find(stream_id);
  if (it == active_tasks_.end() || it->second->file_handle == -1) {
    SendResponse(Protocol::Command::UploadData, 400, stream_id,
                 {{"msg", "stream not found or file not open"}}, {});
    return;
  }
  auto task = it->second;

  if (req.header.binary_len == 0) {
    LOG_INFO("Upload finished signal for stream {}. Pending writes: {}",
             stream_id, task->pending_fs_reqs);
    task->closing_pending = true;

    if (task->pending_fs_reqs == 0) {
      uv_fs_t *close_req = new uv_fs_t();
      close_req->data = new IOCtx{shared_from_this(), stream_id};
      uv_fs_close(socket_.loop, close_req, task->file_handle,
                  Session::OnFileClose);
      task->closing_pending = false;
    }
    return;
  }

  struct WriteCtx {
    std::shared_ptr<Session> session;
    uint32_t stream_id;
    std::vector<char> data;
    uv_buf_t buf;
  };

  WriteCtx *ctx = new WriteCtx();
  ctx->session = shared_from_this();
  ctx->stream_id = stream_id;
  ctx->data = req.binary_payload;
  ctx->buf = uv_buf_init(ctx->data.data(), ctx->data.size());

  uv_fs_t *write_req = new uv_fs_t();
  write_req->data = ctx;

  uint64_t current_write_at = task->file_offset;
  task->file_offset += req.header.binary_len;
  task->pending_fs_reqs++;

  int r = uv_fs_write(
      socket_.loop, write_req, task->file_handle, &ctx->buf, 1, current_write_at,
      [](uv_fs_t *req) {
        WriteCtx *ctx = static_cast<WriteCtx *>(req->data);
        auto session = ctx->session;
        uint32_t sid = ctx->stream_id;

        auto it = session->active_tasks_.find(sid);
        if (it != session->active_tasks_.end()) {
          auto task = it->second;
          if (req->result < 0) {
            LOG_ERROR("uv_fs_write error for stream {}: {}", sid,
                      uv_strerror(req->result));
          }

          task->pending_fs_reqs--;
          if (task->closing_pending && task->pending_fs_reqs == 0) {
            LOG_INFO("Last write finished for stream {}. Closing file.", sid);
            uv_fs_t *close_req = new uv_fs_t();
            close_req->data = new IOCtx{session, sid};
            uv_fs_close(session->socket_.loop, close_req, task->file_handle,
                        Session::OnFileClose);
            task->closing_pending = false;
          }
        }

        uv_fs_req_cleanup(req);
        delete req;
        delete ctx;
      });

  if (r < 0) {
    LOG_ERROR("uv_fs_write start error: {}", uv_strerror(r));
    task->pending_fs_reqs--;
    delete write_req;
    delete ctx;
  }
}

void Session::HandleDownloadReq(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::DownloadData, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  int file_id = req.json_payload.value("file_id", -1);
  uint64_t offset = req.json_payload.value("offset", 0);
  uint32_t stream_id = req.header.stream_id;

  if (file_id == -1) {
    SendResponse(Protocol::Command::DownloadData, 400, stream_id,
                 {{"msg", "missing file_id"}}, {});
    return;
  }

  auto file_info = Database::GetInstance().GetFileById(user_id_, file_id);
  if (file_info.empty()) {
    SendResponse(Protocol::Command::DownloadData, 404, stream_id,
                 {{"msg", "file id not found"}}, {});
    return;
  }

  std::string filename = file_info["filename"];
  std::string full_path = "data/" + std::to_string(user_id_) + "/" + filename;

  auto task = std::make_shared<FileTask>();
  task->stream_id = stream_id;
  task->current_filename = filename;
  task->file_offset = offset;
  task->is_uploading = false;
  active_tasks_[stream_id] = task;

  // Open file for reading
  uv_fs_t *open_req = new uv_fs_t();
  open_req->data = new IOCtx{shared_from_this(), stream_id};

  int r = uv_fs_open(
      socket_.loop, open_req, full_path.c_str(), O_RDONLY, 0, [](uv_fs_t *req) {
        auto *ctx = static_cast<IOCtx *>(req->data);
        auto session = ctx->session;
        uint32_t sid = ctx->stream_id;

        auto it = session->active_tasks_.find(sid);
        if (it == session->active_tasks_.end()) {
          LOG_ERROR("Task not found for stream {} in HandleDownloadReq open",
                    sid);
          delete ctx;
          delete req;
          return;
        }
        auto task = it->second;

        if (req->result >= 0) {
          task->file_handle = req->result;
          uv_fs_t stat_req;
          uint64_t total_size = 0;
          if (uv_fs_fstat(session->socket_.loop, &stat_req, task->file_handle,
                          nullptr) == 0) {
            total_size = stat_req.statbuf.st_size;
            task->total_filesize = total_size;
            session->SendResponse(Protocol::Command::DownloadData, 200, sid,
                                  {{"total_size", total_size},
                                   {"filename", task->current_filename},
                                   {"msg", "start"}},
                                  {});
          }
          uv_fs_req_cleanup(&stat_req);

          LOG_INFO("File opened for reading: {}, size: {}, stream: {}",
                   req->path, total_size, sid);

          // Trigger first read
          uv_buf_t buf = uv_buf_init(task->file_read_buf,
                                     sizeof(task->file_read_buf));
          uv_fs_read(session->socket_.loop, req, task->file_handle, &buf, 1,
                     task->file_offset, Session::OnFileRead);
        } else {
          LOG_ERROR("uv_fs_open error for stream {}: {}", sid,
                    uv_strerror(req->result));
          session->SendResponse(Protocol::Command::DownloadData, 404, sid,
                                {{"msg", "file not found on disk"}}, {});
          session->active_tasks_.erase(sid);
          uv_fs_req_cleanup(req);
          delete ctx;
          delete req;
        }
      });

  if (r < 0) {
    LOG_ERROR("uv_fs_open start error: {}", uv_strerror(r));
    delete static_cast<IOCtx *>(open_req->data);
    delete open_req;
    SendResponse(Protocol::Command::DownloadData, 500, stream_id,
                 {{"msg", "io error"}}, {});
    active_tasks_.erase(stream_id);
  }
}

void Session::OnFileOpen(uv_fs_t *req) {
  auto *ctx = static_cast<IOCtx *>(req->data);
  auto session = ctx->session;
  uint32_t sid = ctx->stream_id;

  auto it = session->active_tasks_.find(sid);
  if (it == session->active_tasks_.end()) {
    LOG_ERROR("Task not found for stream {} in OnFileOpen", sid);
    delete ctx;
    delete req;
    return;
  }
  auto task = it->second;

  if (req->result >= 0) {
    task->file_handle = req->result;
    LOG_INFO("File opened successfully for stream {}, fd: {}, start offset: {}",
             sid, task->file_handle, task->file_offset);
    session->SendResponse(Protocol::Command::UploadReq, 200, sid,
                          {{"msg", "ready"}, {"offset", task->file_offset}},
                          {});
  } else {
    LOG_ERROR("OnFileOpen error for stream {}: {}", sid,
              uv_strerror(req->result));
    session->SendResponse(Protocol::Command::UploadReq, 500, sid,
                          {{"msg", "open failed"}}, {});
    session->active_tasks_.erase(sid);
  }
  uv_fs_req_cleanup(req);
  delete ctx;
  delete req;
}

void Session::OnFileClose(uv_fs_t *req) {
  auto *ctx = static_cast<IOCtx *>(req->data);
  auto session = ctx->session;
  uint32_t sid = ctx->stream_id;

  auto it = session->active_tasks_.find(sid);
  if (it == session->active_tasks_.end()) {
    uv_fs_req_cleanup(req);
    delete ctx;
    delete req;
    return;
  }
  auto task = it->second;

  LOG_INFO("File closed for stream {}, fd: {}", sid, task->file_handle);

  if (task->is_uploading && !task->current_filename.empty()) {
    std::string path =
        "data/" + std::to_string(session->user_id_) + "/" + task->current_filename;
    Database::GetInstance().AddFile(session->user_id_, 0, task->current_filename,
                                    task->total_filesize, false, path);
    LOG_INFO("File metadata synced to database for stream {}: {}", sid,
             task->current_filename);
    session->SendResponse(Protocol::Command::UploadData, 200, sid,
                          {{"msg", "upload complete"}}, {});
  }

  session->active_tasks_.erase(sid);
  uv_fs_req_cleanup(req);
  delete ctx;
  delete req;
}

void Session::OnFileWrite(uv_fs_t *req) {
  // Handled in-line in HandleUploadData for now to simplify
}

void Session::OnFileRead(uv_fs_t *req) {
  auto *ctx = static_cast<IOCtx *>(req->data);
  auto session = ctx->session;
  uint32_t sid = ctx->stream_id;

  auto it = session->active_tasks_.find(sid);
  if (it == session->active_tasks_.end()) {
    LOG_ERROR("Task not found for stream {} in OnFileRead", sid);
    delete ctx;
    delete req;
    return;
  }
  auto task = it->second;

  if (req->result > 0) {
    size_t bytes_read = req->result;
    task->file_offset += bytes_read;

    std::vector<char> data(task->file_read_buf,
                           task->file_read_buf + bytes_read);
    session->SendResponse(Protocol::Command::DownloadData, 200, sid,
                          {{"eof", false}}, data);

    // Continue reading next chunk
    uv_buf_t buf = uv_buf_init(task->file_read_buf, sizeof(task->file_read_buf));
    uv_fs_read(session->socket_.loop, req, task->file_handle, &buf, 1,
               task->file_offset, Session::OnFileRead);
  } else if (req->result == 0) {
    // EOF
    LOG_INFO("Download complete for stream {}.", sid);
    session->SendResponse(Protocol::Command::DownloadData, 200, sid,
                          {{"eof", true}}, {});
    uv_fs_close(session->socket_.loop, req, task->file_handle,
                Session::OnFileClose);
  } else {
    LOG_ERROR("uv_fs_read error for stream {}: {}", sid,
              uv_strerror(req->result));
    session->SendResponse(Protocol::Command::DownloadData, 500, sid,
                          {{"msg", "read error"}}, {});
    uv_fs_close(session->socket_.loop, req, task->file_handle,
                Session::OnFileClose);
  }
}
