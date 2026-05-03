#include "Session.h"
#include "Database.h"
#include "Logger.h"
#include "Config.h"
#include <cstring>
#include <uv.h>

struct IOCtx {
  std::shared_ptr<Session> session;
  uint32_t stream_id;
};

Session::Session(uv_loop_t *loop) {
  uv_tcp_init(loop, &socket_);
  socket_.data = this;
  uv_timer_init(loop, &rate_timer_);
  rate_timer_.data = this;
  uv_timer_start(&rate_timer_, Session::OnRateTimer, 1000, 1000);
}

Session::~Session() {
  uv_timer_stop(&rate_timer_);
  LOG_INFO("Session destroyed.");
}

void Session::OnRateTimer(uv_timer_t *handle) {
    Session *session = static_cast<Session *>(handle->data);
    session->upload_bytes_this_sec_ = 0;
    session->download_bytes_this_sec_ = 0;
    
    // Resume Uploads
    if (session->is_reading_paused_) {
        session->is_reading_paused_ = false;
        uv_read_start((uv_stream_t *)&session->socket_, Session::OnAlloc, Session::OnRead);
    }

    // Resume Downloads
    if (!session->suspended_downloads_.empty()) {
        for (uint32_t sid : session->suspended_downloads_) {
            auto it = session->active_tasks_.find(sid);
            if (it != session->active_tasks_.end()) {
                auto task = it->second;
                uv_buf_t buf = uv_buf_init(task->file_read_buf, sizeof(task->file_read_buf));
                uv_fs_t *read_req = new uv_fs_t();
                read_req->data = new IOCtx{session->shared_from_this(), sid};
                uv_fs_read(session->socket_.loop, read_req, task->file_handle, &buf, 1,
                           task->file_offset, Session::OnFileRead);
            }
        }
        session->suspended_downloads_.clear();
    }
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
    
    // Rate Limiting (Upload)
    session->upload_bytes_this_sec_ += nread;
    int upload_limit_kbps = Config::GetInstance().Get<int>("limits/upload_kbps", 0);
    if (upload_limit_kbps > 0 && session->upload_bytes_this_sec_ >= (uint64_t)upload_limit_kbps * 1024) {
        session->is_reading_paused_ = true;
        uv_read_stop(stream);
    }

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
  case Protocol::Command::ListAllDirs:
    HandleListAllDirs(msg);
    break;
  case Protocol::Command::MakeDir:
    HandleMakeDir(msg);
    break;
  case Protocol::Command::Remove:
    HandleRemove(msg);
    break;
  case Protocol::Command::Move:
    HandleMove(msg);
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
                 msg.header.stream_id, {{"error", "Unknown command"}}, {});
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
    user_key_ = CryptoUtils::DeriveKey(password);
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
    SendResponse(Protocol::Command::ListDir, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  int parent_id = req.json_payload.value("parent_id", 0);
  auto files = Database::GetInstance().ListFiles(user_id_, parent_id);
  SendResponse(Protocol::Command::ListDir, 200, req.header.stream_id,
               {{"files", files}}, {});
}

void Session::HandleListAllDirs(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::ListAllDirs, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  auto dirs = Database::GetInstance().GetAllDirectories(user_id_);
  SendResponse(Protocol::Command::ListAllDirs, 200, req.header.stream_id,
               {{"dirs", dirs}}, {});
}

void Session::HandleMakeDir(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::MakeDir, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  std::string dirname = req.json_payload.value("dirname", "");
  int parent_id = req.json_payload.value("parent_id", 0);

  if (dirname.empty()) {
    SendResponse(Protocol::Command::MakeDir, 400, req.header.stream_id,
                 {{"msg", "missing dirname"}}, {});
    return;
  }

  if (Database::GetInstance().AddFile(user_id_, parent_id, dirname, 0, true, "")) {
    SendResponse(Protocol::Command::MakeDir, 200, req.header.stream_id,
                 {{"msg", "success"}}, {});
  } else {
    SendResponse(Protocol::Command::MakeDir, 500, req.header.stream_id,
                 {{"msg", "db error"}}, {});
  }
}

void Session::HandleMove(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::Move, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  int file_id = req.json_payload.value("file_id", -1);
  int new_parent_id = req.json_payload.value("new_parent_id", -1);

  if (file_id == -1 || new_parent_id == -1) {
    SendResponse(Protocol::Command::Move, 400, req.header.stream_id,
                 {{"msg", "missing file_id or new_parent_id"}}, {});
    return;
  }

  if (Database::GetInstance().MoveFile(user_id_, file_id, new_parent_id)) {
    SendResponse(Protocol::Command::Move, 200, req.header.stream_id,
                 {{"msg", "success"}}, {});
  } else {
    SendResponse(Protocol::Command::Move, 500, req.header.stream_id,
                 {{"msg", "db error"}}, {});
  }
}

void Session::HandleRemove(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::Remove, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  int file_id = req.json_payload.value("file_id", -1);
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

  // If it's a directory, we need to delete all subfiles and subdirectories recursively.
  // We use GetAllSubFiles to get all descendants.
  bool is_dir = false;
  if (file_info.contains("is_dir")) {
      is_dir = file_info["is_dir"].is_boolean() ? file_info["is_dir"].get<bool>() : (file_info["is_dir"].get<int>() != 0);
  }

  std::vector<nlohmann::json> to_delete;
  if (is_dir) {
      to_delete = Database::GetInstance().GetAllSubFiles(user_id_, file_id);
  }
  to_delete.push_back(file_info); // Add the target itself

  for (const auto& item : to_delete) {
      int id = item["id"];
      bool item_is_dir = false;
      if (item.contains("is_dir")) {
          item_is_dir = item["is_dir"].is_boolean() ? item["is_dir"].get<bool>() : (item["is_dir"].get<int>() != 0);
      }
      std::string path = item.value("file_path", "");

      // 1. Delete from database
      Database::GetInstance().DeleteFile(user_id_, id);

      // 2. Delete from filesystem if it's a physical file
      if (!item_is_dir && !path.empty()) {
          uv_fs_t unlink_req;
          uv_fs_unlink(socket_.loop, &unlink_req, path.c_str(), nullptr);
          uv_fs_req_cleanup(&unlink_req);
      }
  }

  SendResponse(Protocol::Command::Remove, 200, req.header.stream_id,
               {{"msg", "success"}}, {});
}

void Session::HandleUploadReq(const Protocol::Message &req) {
  if (user_id_ == -1) {
    SendResponse(Protocol::Command::UploadReq, 403, req.header.stream_id,
                 {{"msg", "not logged in"}}, {});
    return;
  }

  std::string filename = req.json_payload.value("filename", "unknown");
  size_t total_size = req.json_payload.value("filesize", 0);
  int parent_id = req.json_payload.value("parent_id", 0);
  uint32_t stream_id = req.header.stream_id;

  LOG_INFO("Client wants to upload file: {}, total_size: {}, parent_id: {}, stream: {}",
           filename, total_size, parent_id, stream_id);

  std::string user_dir = "data/" + std::to_string(user_id_);
  std::string full_path = user_dir + "/" + filename;

  // 1. 检查物理文件是否已存在（断点续传寻址）
  uint64_t current_offset = 0;
  struct stat st;
  if (stat(full_path.c_str(), &st) == 0) {
      current_offset = st.st_size;
      LOG_INFO("Physical file exists for upload. Found offset: {} for file: {}", current_offset, filename);
  }

  auto task = std::make_shared<FileTask>();
  task->stream_id = stream_id;
  task->current_filename = filename;
  task->full_path = full_path;
  task->parent_id = parent_id;
  task->total_filesize = total_size;
  task->is_uploading = true;
  task->file_offset = current_offset; // 设置起始偏移量
  active_tasks_[stream_id] = task;

  // 1. Ensure user directory exists
  uv_fs_t mkdir_req;
  uv_fs_mkdir(socket_.loop, &mkdir_req, user_dir.c_str(), 0755, nullptr);
  uv_fs_req_cleanup(&mkdir_req);

  // 2. Check if file is already complete

  if (current_offset >= total_size && total_size > 0) {
    auto existing = Database::GetInstance().GetFile(user_id_, parent_id, filename);
    if (existing.empty() || existing["filesize"] != (int64_t)current_offset) {
      Database::GetInstance().AddFile(user_id_, parent_id, filename, current_offset,
                                      false, full_path);
    }
    LOG_INFO("File already fully uploaded. Offset: {}, Total: {}",
             current_offset, total_size);
    SendResponse(Protocol::Command::UploadReq, 200, stream_id,
                 {{"offset", current_offset},
                  {"status", "complete"},
                  {"msg", "file already complete"}},
                 {});
    active_tasks_.erase(stream_id);
    return;
  }

  // File is new or partially uploaded. We will add it to DB only when upload finishes.

  // 3. Open file for writing
  task->file_offset = current_offset;
  uv_fs_t *open_req = new uv_fs_t();
  open_req->data = new IOCtx{shared_from_this(), stream_id};

  int r = uv_fs_open(socket_.loop, open_req, full_path.c_str(), O_WRONLY | O_CREAT,
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
  
  if (!user_key_.empty()) {
      CryptoUtils::ProcessCTR(user_key_, task->file_offset, ctx->data);
  }
  
  ctx->buf = uv_buf_init(ctx->data.data(), ctx->data.size());

  uv_fs_t *write_req = new uv_fs_t();
  write_req->data = ctx;

  uint64_t current_write_at = task->file_offset;
  task->file_offset += req.header.binary_len;
  task->pending_fs_reqs++;

  int r = uv_fs_write(
      socket_.loop, write_req, task->file_handle, &ctx->buf, 1,
      current_write_at, [](uv_fs_t *req) {
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
  if (req.json_payload.value("abort", false)) {
    uint32_t sid = req.header.stream_id;
    auto it = active_tasks_.find(sid);
    if (it != active_tasks_.end()) {
      auto task = it->second;
      if (task->file_handle >= 0) {
        uv_fs_t *close_req = new uv_fs_t();
        close_req->data = new IOCtx{shared_from_this(), sid};
        uv_fs_close(socket_.loop, close_req, task->file_handle, Session::OnFileClose);
        task->file_handle = -1; // Prevent double close in pending reads
      } else {
        active_tasks_.erase(sid);
      }
      LOG_INFO("Download aborted by client for stream {}", sid);
    }
    return;
  }

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
  std::string path = file_info["file_path"];
  int64_t filesize = file_info["filesize"];

  auto task = std::make_shared<FileTask>();
  task->stream_id = stream_id;
  task->total_filesize = filesize;
  task->current_filename = filename;
  task->full_path = path;
  task->file_offset = offset;
  task->is_uploading = false;
  active_tasks_[stream_id] = task;

  // Open file for reading
  uv_fs_t *open_req = new uv_fs_t();
  open_req->data = new IOCtx{shared_from_this(), stream_id};

  int r = uv_fs_open(
      socket_.loop, open_req, path.c_str(), O_RDONLY, 0, [](uv_fs_t *req) {
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

          uv_fs_req_cleanup(req); // MUST cleanup before reuse

          // Trigger first read
          uv_buf_t buf =
              uv_buf_init(task->file_read_buf, sizeof(task->file_read_buf));
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

    // If this was an upload, now is the time to add it to the database
    if (task->is_uploading) {
        if (Database::GetInstance().AddFile(session->user_id_, task->parent_id, task->current_filename, 
                                            task->total_filesize, false, task->full_path)) {
            LOG_INFO("File {} added to database after successful upload.", 
                     task->current_filename);
        } else {
            LOG_ERROR("Failed to add file {} to database after upload.", task->current_filename);
        }
    }

    LOG_INFO("File closed for stream {}, fd: {}", sid, task->file_handle);
    
    if (task->is_uploading) {
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

    if (!session->user_key_.empty()) {
        CryptoUtils::ProcessCTR(session->user_key_, task->file_offset - bytes_read, data);
    }

    session->SendResponse(Protocol::Command::DownloadData, 200, sid,
                          {{"eof", false}}, data);

    uv_fs_req_cleanup(req); // MUST cleanup before reuse

    // Rate Limiting (Download)
    session->download_bytes_this_sec_ += bytes_read;
    int download_limit_kbps = Config::GetInstance().Get<int>("limits/download_kbps", 0);
    
    if (download_limit_kbps > 0 && session->download_bytes_this_sec_ >= (uint64_t)download_limit_kbps * 1024) {
        // Suspend this stream. It will be resumed by OnRateTimer.
        session->suspended_downloads_.insert(sid);
        uv_fs_req_cleanup(req);
        delete req; // In this case we delete the req because a new one will be created upon resume
        return;
    }

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
