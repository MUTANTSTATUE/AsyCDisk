#include "AsyCClient.h"
#include <arpa/inet.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sys/socket.h>
#include <sys/poll.h>
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
  
  // Wake up all waiting streams
  {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    for (auto &pair : streams_) {
      std::lock_guard<std::mutex> q_lock(pair.second->mtx);
      pair.second->closed = true;
      pair.second->cv.notify_all();
    }
  }

  if (sock_ >= 0) {
    shutdown(sock_, SHUT_RDWR);
    close(sock_);
    sock_ = -1;
  }
  
  if (receiver_thread_.joinable()) {
    receiver_thread_.join();
  }

  // Join all worker threads
  std::lock_guard<std::mutex> lock(workers_mutex_);
  for (auto &t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
}

void AsyCClient::CreateStream(uint32_t sid) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  streams_[sid] = std::make_shared<StreamContext>();
}

void AsyCClient::DeleteStream(uint32_t sid) {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  streams_.erase(sid);
}

void AsyCClient::AddWorker(std::thread &&t) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.push_back(std::move(t));
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
  if (sock_ < 0) return false;
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
  struct pollfd pfd;
  pfd.fd = sock_;
  pfd.events = POLLIN;

  while (running_) {
    int ret = poll(&pfd, 1, 50); // 50ms timeout to check running_ flag
    if (ret < 0) return false;
    if (ret == 0) continue; // Timeout, check running_ and loop
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    break; 
  }
  if (!running_) return false;

  if (recv(sock_, &msg.header, sizeof(msg.header), MSG_WAITALL) !=
      sizeof(msg.header))
    return false;
  if (msg.header.magic != Protocol::MAGIC_NUMBER)
    return false;

  if (msg.header.json_len > 0) {
    std::string j_str(msg.header.json_len, 0);
    recv(sock_, &j_str[0], msg.header.json_len, MSG_WAITALL);
    try {
        msg.json_payload = json::parse(j_str);
    } catch (...) {
        return false;
    }
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
    
    // 捕获“被踢下线”消息 (登录响应且状态码为 403)
    if (msg.header.command == static_cast<uint16_t>(Protocol::Command::Login) && 
        msg.header.status == 403) {
        if (on_kicked_) on_kicked_();
        continue;
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
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) return {};
    ctx = it->second;
  }

  std::unique_lock<std::mutex> q_lock(ctx->mtx);
  ctx->cv.wait(q_lock, [&] { return !ctx->messages.empty() || ctx->closed || ctx->aborted || !running_; });

  if (!running_ || ctx->closed || ctx->aborted || ctx->messages.empty())
    return {}; 

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
    success = true;
    current_user_ = user;
    current_user_id_ = msg.json_payload.value("user_id", -1);
    std::cout << "[OK] Login successful for " << user << " (ID: " << current_user_id_ << ")" << std::endl;
  }
  DeleteStream(sid);
  return success;
}

bool AsyCClient::Register(const std::string &user, const std::string &pass) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  if (!SendPacket(Protocol::Command::Register, sid,
                  {{"username", user}, {"password", pass}})) {
    DeleteStream(sid);
    return false;
  }
  
  auto msg = WaitNextMessage(sid);
  bool success = false;
  if (msg.header.magic != 0 && msg.header.status == 200) {
    std::cout << "[OK] Registration successful." << std::endl;
    success = true;
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

json AsyCClient::Search(const std::string &keyword) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);
  if (!SendPacket(Protocol::Command::Search, sid, {{"keyword", keyword}})) {
    DeleteStream(sid);
    return {};
  }
  
  auto msg = WaitNextMessage(sid);
  json result = json::array();
  if (msg.header.magic != 0 && msg.header.status == 200) {
    result = msg.json_payload.value("files", json::array());
  }
  DeleteStream(sid);
  return result;
}

void AsyCClient::Upload(const std::string &local_path, int parent_id,
                        std::function<void(uint32_t sid, uint64_t cur, uint64_t total)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);

  AddWorker(std::thread([this, local_path, parent_id, sid, cb]() {
    std::ifstream file(local_path, std::ios::binary);
    if (!file) {
      DeleteStream(sid);
      return;
    }

    std::string filename = local_path.substr(local_path.find_last_of("/\\") + 1);
    file.seekg(0, std::ios::end);
    size_t filesize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (!SendPacket(Protocol::Command::UploadReq, sid,
                    {{"filename", filename}, {"filesize", filesize}, {"parent_id", parent_id}})) {
      DeleteStream(sid);
      return;
    }

    auto msg_init = WaitNextMessage(sid);
    if (msg_init.header.magic == 0 || msg_init.header.status != 200) {
      DeleteStream(sid);
      return;
    }

    uint64_t offset = msg_init.json_payload.value("offset", 0);
    file.seekg(offset);
    
    uint64_t uploaded = offset;
    char buf[65536];
    while (uploaded < filesize && running_) {
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
                SendPacket(Protocol::Command::UploadData, sid, {{"abort", true}}, {});
                break;
            }
            ctx->cv.wait(lock, [ctx, this] { return !ctx->paused || ctx->aborted || !running_; });
            if (ctx->aborted || !running_) break;
        } else break;
      }

      uploaded += read;
      if (cb) cb(sid, uploaded, filesize);
    }

    if (running_) {
        SendPacket(Protocol::Command::UploadData, sid, {}, {});
        WaitNextMessage(sid);
    }
    DeleteStream(sid);
  }));
}

void AsyCClient::Download(int file_id, const std::string &local_path,
                          std::function<void(uint32_t sid, uint64_t cur, uint64_t total)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);

  AddWorker(std::thread([this, file_id, local_path, sid, cb]() {
    uint64_t offset = 0;
    std::string target_path = local_path;
    std::string tmp_path = target_path + ".tmp";
    std::string meta_path = target_path + ".tmp.meta";

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
    
    {
        std::ofstream meta_f(meta_path);
        if (meta_f) {
            json meta_j = {{"i", file_id}, {"s", filesize}, {"u", current_user_id_}, {"n", current_user_}};
            meta_f << meta_j.dump();
        }
    }

    std::ofstream file;
    if (offset > 0) {
        file.open(tmp_path, std::ios::binary | std::ios::app);
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

    while (downloaded < filesize && running_) {
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
              ctx->cv.wait(lock, [ctx, this] { return !ctx->paused || ctx->aborted || !running_; });
              if (ctx->aborted || !running_) { aborted_internally = true; break; }
          } else break;
      }
      
      file.write(msg.binary_payload.data(), msg.binary_payload.size());
      downloaded += msg.binary_payload.size();
      if (cb) cb(sid, downloaded, filesize);
    }
    
    file.close(); 
    if (!aborted_internally && downloaded >= filesize) {
        std::rename(tmp_path.c_str(), target_path.c_str());
        std::remove(meta_path.c_str());
        if (cb) cb(sid, filesize, filesize); 
    }
    DeleteStream(sid);
  }));
}

uint32_t AsyCClient::StreamDownload(int file_id, uint64_t offset,
                                std::function<bool(const std::vector<char>& chunk, uint64_t total_size, const std::string& filename, bool is_eof)> cb) {
  uint32_t sid = next_stream_id_++;
  CreateStream(sid);

  AddWorker(std::thread([this, file_id, offset, sid, cb]() {
    ReceiverLoop_Stream(file_id, offset, sid, cb);
    DeleteStream(sid);
  }));
  return sid;
}

void AsyCClient::ReceiverLoop_Stream(int file_id, uint64_t offset, uint32_t sid,
                                     std::function<bool(const std::vector<char>& chunk, uint64_t total_size, const std::string& filename, bool is_eof)> cb) {
    if (!SendPacket(Protocol::Command::DownloadReq, sid, {{"file_id", file_id}, {"offset", offset}})) {
      return;
    }

    auto msg_init = WaitNextMessage(sid);
    if (msg_init.header.magic == 0 || msg_init.header.status != 200) {
      return;
    }

    std::string filename = msg_init.json_payload.value("filename", "unknown");
    uint64_t filesize = msg_init.json_payload.value("filesize", 
                          msg_init.json_payload.value("total_size", (uint64_t)0));
    
    while (running_) {
      auto msg = WaitNextMessage(sid);
      bool is_eof = (msg.header.magic == 0 || msg.header.binary_len == 0);
      
      if (cb) {
        if (!cb(msg.binary_payload, filesize, filename, is_eof)) {
          if (!is_eof && running_) {
            SendPacket(Protocol::Command::DownloadReq, sid, {{"abort", true}});
          }
          break; 
        }
      }
      if (is_eof) break;
    }
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
  
  AddWorker(std::thread([this, file_id, sid, cb]() {
    if (!SendPacket(Protocol::Command::Remove, sid, {{"file_id", file_id}})) {
      if (cb) cb(false, "Failed to send request");
      DeleteStream(sid);
      return;
    }

    auto msg = WaitNextMessage(sid);
    if (msg.header.magic != 0 && msg.header.status == 200) {
      if (cb) cb(true, "Deleted successfully");
    } else {
      if (cb) cb(false, msg.json_payload.value("msg", "Unknown error"));
    }
    DeleteStream(sid);
  }));
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
            std::string tmp_path = meta_path.substr(0, meta_path.size() - 5); 
            std::string original_path = tmp_path.substr(0, tmp_path.size() - 4); 

            std::ifstream meta_f(meta_path);
            if (meta_f) {
                try {
                    json meta_j;
                    meta_f >> meta_j;
                    
                    IncompleteTask task;
                    task.file_id = meta_j.value("i", 0);
                    task.total_size = meta_j.value("s", (uint64_t)0);
                    task.user_id = meta_j.value("u", -1);
                    task.username = meta_j.value("n", "");
                    task.filename = filename.substr(0, filename.size() - 9); 
                    task.local_path = original_path;
                    
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
