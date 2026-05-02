#include "AsyCClient.h"
#include <arpa/inet.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

AsyCClient::AsyCClient(const std::string &ip, uint16_t port) : ip_(ip), port_(port) {}

AsyCClient::~AsyCClient() {
  Close();
}

void AsyCClient::ShowProgressBar(uint64_t current, uint64_t total) {
  const int barWidth = 40;
  float progress = (total > 0) ? (float)current / total : 0;
  if (progress > 1.0)
    progress = 1.0;

  std::cout << "\r[";
  int pos = barWidth * progress;
  for (int i = 0; i < barWidth; ++i) {
    if (i < pos)
      std::cout << "#";
    else
      std::cout << "-";
  }
  std::cout << "] " << int(progress * 100.0) << "% (" << current / 1024
            << " / " << total / 1024 << " KB) " << std::flush;
  if (current >= total)
    std::cout << std::endl;
}

std::string AsyCClient::FormatSize(uint64_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int unitIndex = 0;
  double size = (double)bytes;
  while (size >= 1024 && unitIndex < 4) {
    size /= 1024;
    unitIndex++;
  }
  std::stringstream ss;
  ss << std::fixed << std::setprecision(2) << size << " " << units[unitIndex];
  return ss.str();
}

bool AsyCClient::Connect() {
  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0)
    return false;

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

  if (connect(sock_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    return false;
  }

  running_ = true;
  receiver_thread_ = std::thread(&AsyCClient::ReceiverLoop, this);
  return true;
}

void AsyCClient::Close() {
  running_ = false;
  if (sock_ >= 0) {
    shutdown(sock_, SHUT_RDWR);
    close(sock_);
    sock_ = -1;
  }
  if (receiver_thread_.joinable()) {
    receiver_thread_.join();
  }
}

void AsyCClient::CreateStream(uint32_t sid) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  streams_[sid] = std::make_shared<StreamContext>();
}

void AsyCClient::DeleteStream(uint32_t sid) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  streams_.erase(sid);
}

void AsyCClient::AbortStream(uint32_t sid) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  if (streams_.count(sid)) {
    streams_[sid]->aborted = true;
    streams_[sid]->cv.notify_all();
  }
}

void AsyCClient::PauseStream(uint32_t sid) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  if (streams_.count(sid)) {
    streams_[sid]->paused = true;
  }
}

void AsyCClient::ResumeStream(uint32_t sid) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  if (streams_.count(sid)) {
    streams_[sid]->paused = false;
    streams_[sid]->cv.notify_all();
  }
}

bool AsyCClient::SendPacket(Protocol::Command cmd, uint32_t stream_id,
                            const json &j_payload,
                            const std::vector<char> &b_payload) {
  std::string j_str;
  if (!j_payload.is_null() && !j_payload.empty()) {
    j_str = j_payload.dump();
  }

  Protocol::Header header;
  header.magic = Protocol::MAGIC_NUMBER;
  header.version = Protocol::CURRENT_VERSION;
  header.command = static_cast<uint16_t>(cmd);
  header.status = 0;
  header.stream_id = stream_id;
  header.json_len = j_str.size();
  header.binary_len = b_payload.size();

  std::lock_guard<std::mutex> lock(send_mutex_);
  if (send(sock_, &header, sizeof(header), 0) != sizeof(header))
    return false;
  if (header.json_len > 0) {
    if (send(sock_, j_str.data(), j_str.size(), 0) != (ssize_t)j_str.size())
      return false;
  }
  if (header.binary_len > 0) {
    if (send(sock_, b_payload.data(), b_payload.size(), 0) !=
        (ssize_t)b_payload.size())
      return false;
  }
  return true;
}

bool AsyCClient::RecvPacket(Protocol::Message &msg) {
  if (recv(sock_, &msg.header, sizeof(msg.header), MSG_WAITALL) !=
      sizeof(msg.header))
    return false;
  if (msg.header.magic != Protocol::MAGIC_NUMBER)
    return false;

  if (msg.header.json_len > 0) {
    std::string j_str(msg.header.json_len, 0);
    recv(sock_, &j_str[0], msg.header.json_len, MSG_WAITALL);
    msg.json_payload = json::parse(j_str);
  }
  if (msg.header.binary_len > 0) {
    msg.binary_payload.resize(msg.header.binary_len);
    recv(sock_, &msg.binary_payload[0], msg.header.binary_len, MSG_WAITALL);
  }
  return true;
}

void AsyCClient::ReceiverLoop() {
  while (running_) {
    Protocol::Message msg;
    if (!RecvPacket(msg)) {
      if (running_) {
        std::cout << "\n[ERR] Connection lost." << std::endl;
        running_ = false;
      }
      break;
    }

    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(msg.header.stream_id);
    if (it != streams_.end()) {
      std::lock_guard<std::mutex> q_lock(it->second->mtx);
      it->second->messages.push(std::move(msg));
      it->second->cv.notify_one();
    }
  }
}

Protocol::Message AsyCClient::WaitNextMessage(uint32_t stream_id) {
  std::shared_ptr<StreamContext> ctx;
  {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    ctx = streams_[stream_id];
  }

  std::unique_lock<std::mutex> q_lock(ctx->mtx);
  ctx->cv.wait(q_lock, [&] { return !ctx->messages.empty() || ctx->closed; });

  if (ctx->messages.empty())
    return {}; // Stream closed

  Protocol::Message msg = std::move(ctx->messages.front());
  ctx->messages.pop();
  return msg;
}

bool AsyCClient::Login(const std::string &user, const std::string &pass) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  if (!SendPacket(Protocol::Command::Login, sid,
                  {{"username", user}, {"password", pass}})) {
    DeleteStream(sid);
    return false;
  }
  
  auto msg = WaitNextMessage(sid);
  bool success = false;
  if (msg.header.magic != 0 && msg.header.status == 200) {
    std::cout << "[OK] Login successful. UserID: " << msg.json_payload["user_id"]
              << std::endl;
    success = true;
    current_user_ = user;
  } else {
    std::cout << "[ERR] Login failed: " << msg.json_payload.value("msg", "unknown error")
              << std::endl;
  }
  DeleteStream(sid);
  return success;
}

json AsyCClient::List(int parent_id) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  if (!SendPacket(Protocol::Command::ListDir, sid, {{"parent_id", parent_id}})) {
    DeleteStream(sid);
    return {};
  }

  auto msg = WaitNextMessage(sid);
  json result = {};
  if (msg.header.magic != 0 && msg.header.status == 200) {
    result = msg.json_payload["files"];
  }
  DeleteStream(sid);
  return result;
}

json AsyCClient::GetAllDirs() {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  if (!SendPacket(Protocol::Command::ListAllDirs, sid, {})) {
    DeleteStream(sid);
    return {};
  }

  auto msg = WaitNextMessage(sid);
  json result = {};
  if (msg.header.magic != 0 && msg.header.status == 200) {
    result = msg.json_payload["dirs"];
  }
  DeleteStream(sid);
  return result;
}

void AsyCClient::Upload(const std::string &local_path, int parent_id,
                        std::function<void(uint32_t sid, uint64_t cur, uint64_t total)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);

  std::thread([this, local_path, parent_id, sid, cb]() {
    std::ifstream file(local_path, std::ios::binary);
    if (!file) {
      std::cout << "\n[Stream #" << sid << " ERR] Cannot open local file: " << local_path << std::endl;
      DeleteStream(sid);
      return;
    }

    std::string filename = local_path.substr(local_path.find_last_of("/\\") + 1);
    file.seekg(0, std::ios::end);
    size_t filesize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::cout << "\n[Stream #" << sid << " INFO] Starting upload: " << filename << " (" << filesize << " bytes)" << std::endl;

    if (!SendPacket(Protocol::Command::UploadReq, sid,
                    {{"filename", filename}, {"filesize", filesize}, {"parent_id", parent_id}})) {
      DeleteStream(sid);
      return;
    }

    auto msg_init = WaitNextMessage(sid);
    if (msg_init.header.magic == 0 || msg_init.header.status != 200) {
      std::cout << "\n[Stream #" << sid << " ERR] Upload rejected: " << msg_init.json_payload.value("msg", "unknown error") << std::endl;
      DeleteStream(sid);
      return;
    }

    uint64_t offset = msg_init.json_payload.value("offset", 0);
    file.seekg(offset);
    
    uint64_t uploaded = offset;
    char buf[65536];
    while (uploaded < filesize) {
      file.read(buf, sizeof(buf));
      size_t read = file.gcount();
      if (read <= 0) break;

      std::vector<char> chunk(buf, buf + read);
      if (!SendPacket(Protocol::Command::UploadData, sid, {}, chunk))
        break;
      
      {
        std::unique_lock<std::mutex> lock(streams_mutex_);
        if (streams_.count(sid)) {
            auto ctx = streams_[sid];
            if (ctx->aborted) {
                std::cout << "\n[Stream #" << sid << " ABORT] Upload aborted by user." << std::endl;
                SendPacket(Protocol::Command::UploadData, sid, {{"abort", true}}, {});
                break;
            }
            // Wait if paused
            ctx->cv.wait(lock, [ctx] { return !ctx->paused || ctx->aborted; });
            if (ctx->aborted) break;
        }
      }

      uploaded += read;
      if (cb) cb(sid, uploaded, filesize);
    }
    // Check if aborted before finalization
    bool was_aborted = false;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        if (streams_.count(sid) && streams_[sid]->aborted) was_aborted = true;
    }

    if (!was_aborted) {
        // Send completion signal (empty binary)
        SendPacket(Protocol::Command::UploadData, sid, {}, {});
        
        auto msg_done = WaitNextMessage(sid);
        if (msg_done.header.status == 200) {
            if (cb) cb(sid, filesize, filesize);
            std::cout << "\n[Stream #" << sid << " OK] Upload finished: " << filename << std::endl;
        } else {
            std::cout << "\n[Stream #" << sid << " ERR] Upload error at finalization." << std::endl;
        }
    } else {
        std::cout << "\n[Stream #" << sid << " INFO] Upload loop exited due to abort." << std::endl;
    }

    DeleteStream(sid);
  }).detach();

  std::cout << "[INFO] Upload started in background (Stream #" << sid << ")" << std::endl;
}

void AsyCClient::Download(int file_id, const std::string &local_path,
                          std::function<void(uint32_t sid, uint64_t cur, uint64_t total)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);

  std::thread([this, file_id, local_path, sid, cb]() {
    uint64_t offset = 0;
    std::string target_path = local_path;
    std::string tmp_path = target_path + ".tmp";
    std::string meta_path = target_path + ".tmp.meta";

    // 尝试从 .tmp 文件找进度
    std::ifstream existing(tmp_path, std::ios::binary | std::ios::ate);
    if (existing) {
        offset = existing.tellg();
        existing.close();
    }

    json req = {{"file_id", file_id}};
    if (offset > 0) req["offset"] = offset;

    if (!SendPacket(Protocol::Command::DownloadReq, sid, req)) {
      DeleteStream(sid);
      return;
    }

    auto msg_init = WaitNextMessage(sid);
    if (msg_init.header.magic == 0 || msg_init.header.status != 200) {
      DeleteStream(sid);
      return;
    }

    std::string filename = msg_init.json_payload.value("filename", "downloaded_file");
    if (target_path.empty()) {
        target_path = filename;
        tmp_path = target_path + ".tmp";
        meta_path = target_path + ".tmp.meta";
    }

    uint64_t filesize = msg_init.json_payload.value("filesize", 
                          msg_init.json_payload.value("total_size", (uint64_t)0));
    
    // 创建/更新元数据文件
    {
        std::ofstream meta_f(meta_path);
        if (meta_f) {
            json meta_j = {
                {"file_id", file_id},
                {"filename", filename},
                {"username", current_user_}, // 保存用户名
                {"total_size", filesize}
            };
            meta_f << meta_j.dump();
        }
    }

    // 打开 .tmp 文件
    std::ofstream file;
    if (offset > 0) {
        file.open(tmp_path, std::ios::binary | std::ios::app);
        std::cout << "[Stream #" << sid << " INFO] Resuming download to .tmp: " << offset << " / " << filesize << std::endl;
    } else {
        file.open(tmp_path, std::ios::binary);
    }

    if (!file) {
      DeleteStream(sid);
      return;
    }

    uint64_t downloaded = offset;
    bool aborted_internally = false;
    if (cb) cb(sid, downloaded, filesize);

    while (downloaded < filesize) {
      auto msg = WaitNextMessage(sid);
      if (msg.header.magic == 0) break;
      
      {
          std::unique_lock<std::mutex> lock(streams_mutex_);
          if (streams_.count(sid)) {
              auto ctx = streams_[sid];
              if (ctx->aborted) {
                  SendPacket(Protocol::Command::DownloadReq, sid, {{"abort", true}});
                  aborted_internally = true;
                  break;
              }
              ctx->cv.wait(lock, [ctx] { return !ctx->paused || ctx->aborted; });
              if (ctx->aborted) { aborted_internally = true; break; }
          }
      }
      
      file.write(msg.binary_payload.data(), msg.binary_payload.size());
      downloaded += msg.binary_payload.size();
      if (cb) cb(sid, downloaded, filesize);
    }
    
    file.close(); 
    if (aborted_internally) {
        // 中断不删除 .tmp，保留以供下次续传
        std::cout << "[Stream #" << sid << " INFO] Download paused/aborted, kept .tmp file." << std::endl;
    } else if (downloaded >= filesize) {
        // 完成：转正并删除元数据
        if (std::rename(tmp_path.c_str(), target_path.c_str()) == 0) {
            std::remove(meta_path.c_str());
            std::cout << "[Stream #" << sid << " INFO] Download complete, renamed to: " << target_path << std::endl;
        }
        if (cb) cb(sid, filesize, filesize); 
    }
    
    DeleteStream(sid);
  }).detach();
}

void AsyCClient::StreamDownload(int file_id, uint64_t offset,
                                std::function<bool(const std::vector<char>& chunk, uint64_t total_size, const std::string& filename, bool is_eof)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);

  std::thread([this, file_id, offset, sid, cb]() {
    if (!SendPacket(Protocol::Command::DownloadReq, sid, {{"file_id", file_id}, {"offset", offset}})) {
      DeleteStream(sid);
      return;
    }

    auto msg_init = WaitNextMessage(sid);
    if (msg_init.header.magic == 0 || msg_init.header.status != 200) {
      DeleteStream(sid);
      return;
    }

    std::string filename = msg_init.json_payload.value("filename", "unknown");
    uint64_t filesize = msg_init.json_payload.value("filesize", 
                          msg_init.json_payload.value("total_size", (uint64_t)0));
    
    // We don't track downloaded bytes since this is just a stream pass-through.
    // The server will stop sending when it reaches EOF.
    while (true) {
      auto msg = WaitNextMessage(sid);
      bool is_eof = (msg.header.magic == 0 || msg.header.binary_len == 0);
      
      if (cb) {
        if (!cb(msg.binary_payload, filesize, filename, is_eof)) {
          if (!is_eof) {
            // Callback returned false, meaning client disconnected or aborted
            SendPacket(Protocol::Command::DownloadReq, sid, {{"abort", true}});
          }
          break; 
        }
      }
      
      if (is_eof) break;
    }
    
    DeleteStream(sid);
  }).detach();
}

void AsyCClient::MakeDir(int parent_id, const std::string &dirname, 
                         std::function<void(bool success, std::string message)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  if (!SendPacket(Protocol::Command::MakeDir, sid, {{"parent_id", parent_id}, {"dirname", dirname}})) {
    if (cb) cb(false, "Network error");
    DeleteStream(sid);
    return;
  }

  auto msg = WaitNextMessage(sid);
  if (cb) {
    bool success = (msg.header.magic != 0 && msg.header.status == 200);
    cb(success, msg.json_payload.value("msg", "unknown error"));
  }
  DeleteStream(sid);
}

void AsyCClient::Move(int file_id, int new_parent_id, 
                      std::function<void(bool success, std::string message)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  if (!SendPacket(Protocol::Command::Move, sid, {{"file_id", file_id}, {"new_parent_id", new_parent_id}})) {
    if (cb) cb(false, "Network error");
    DeleteStream(sid);
    return;
  }

  auto msg = WaitNextMessage(sid);
  if (cb) {
    bool success = (msg.header.magic != 0 && msg.header.status == 200);
    cb(success, msg.json_payload.value("msg", "unknown error"));
  }
  DeleteStream(sid);
}

void AsyCClient::Remove(int file_id, std::function<void(bool success, std::string message)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  
  std::thread([this, file_id, sid, cb]() {
    if (!SendPacket(Protocol::Command::Remove, sid, {{"file_id", file_id}})) {
      if (cb) cb(false, "Failed to send request");
      DeleteStream(sid);
      return;
    }

    auto msg = WaitNextMessage(sid);
    if (msg.header.magic != 0 && msg.header.status == 200) {
      if (cb) cb(true, "Deleted successfully");
    } else {
      std::string err = msg.json_payload.value("msg", "Unknown error");
      if (cb) cb(false, err);
    }
    DeleteStream(sid);
  }).detach();
}
#include <dirent.h>
#include <sys/stat.h>

std::vector<AsyCClient::IncompleteTask> AsyCClient::ScanIncompleteDownloads(const std::string &directory) {
    std::vector<IncompleteTask> tasks;
    DIR *dir = opendir(directory.c_str());
    if (!dir) return tasks;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        std::string filename = ent->d_name;
        if (filename.size() > 9 && filename.substr(filename.size() - 9) == ".tmp.meta") {
            std::string meta_path = (directory == "." ? "" : directory + "/") + filename;
            std::string tmp_path = meta_path.substr(0, meta_path.size() - 5); // 移除 .meta
            std::string original_path = tmp_path.substr(0, tmp_path.size() - 4); // 移除 .tmp

            std::ifstream meta_f(meta_path);
            if (meta_f) {
                try {
                    json meta_j;
                    meta_f >> meta_j;
                    
                    IncompleteTask task;
                    task.file_id = meta_j["file_id"];
                    task.filename = meta_j["filename"];
                    task.username = meta_j.value("username", ""); // 读取用户名
                    task.total_size = meta_j["total_size"];
                    task.local_path = original_path;
                    
                    // 获取 .tmp 文件实际大小
                    struct stat st;
                    if (stat(tmp_path.c_str(), &st) == 0) {
                        task.current_offset = st.st_size;
                    } else {
                        task.current_offset = 0;
                    }
                    
                    tasks.push_back(task);
                } catch (...) {}
            }
        }
    }
    closedir(dir);
    return tasks;
}
