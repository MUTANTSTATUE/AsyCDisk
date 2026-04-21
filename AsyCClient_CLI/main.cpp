#include "Protocol.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>

using json = nlohmann::json;

struct StreamContext {
  std::queue<Protocol::Message> messages;
  std::mutex mtx;
  std::condition_variable cv;
  bool closed = false;
};

class AsyCClient {
public:
  AsyCClient(const std::string &ip, uint16_t port) : ip_(ip), port_(port) {}

  void ShowProgressBar(uint64_t current, uint64_t total) {
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

  std::string FormatSize(uint64_t bytes) {
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

  bool Connect() {
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

  void Close() {
    running_ = false;
    if (sock_ >= 0) {
      shutdown(sock_, SHUT_RDWR);
      close(sock_);
    }
    if (receiver_thread_.joinable()) {
      receiver_thread_.join();
    }
  }

  void CreateStream(uint32_t sid) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams_[sid] = std::make_shared<StreamContext>();
  }

  void DeleteStream(uint32_t sid) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    streams_.erase(sid);
  }

  bool SendPacket(Protocol::Command cmd, uint32_t stream_id,
                  const json &j_payload,
                  const std::vector<char> &b_payload = {}) {
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

  bool RecvPacket(Protocol::Message &msg) {
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

  void ReceiverLoop() {
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

  Protocol::Message WaitNextMessage(uint32_t stream_id) {
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

  void Login(const std::string &user, const std::string &pass) {
    uint32_t sid = next_stream_id_++;
    CreateStream(sid);
    if (!SendPacket(Protocol::Command::Login, sid,
                    {{"username", user}, {"password", pass}})) {
      DeleteStream(sid);
      return;
    }
    
    auto msg = WaitNextMessage(sid);
    if (msg.header.magic != 0 && msg.header.status == 200) {
      std::cout << "[OK] Login successful. UserID: " << msg.json_payload["user_id"]
                << std::endl;
    } else {
      std::cout << "[ERR] Login failed: " << msg.json_payload.value("msg", "unknown error")
                << std::endl;
    }
    DeleteStream(sid);
  }

  void List() {
    uint32_t sid = next_stream_id_++;
    CreateStream(sid);
    if (!SendPacket(Protocol::Command::ListDir, sid, {{"parent_id", 0}})) {
      DeleteStream(sid);
      return;
    }

    auto msg = WaitNextMessage(sid);
    if (msg.header.magic != 0 && msg.header.status == 200) {
      std::cout << std::left << std::setw(8) << "ID" << std::setw(42)
                << "Filename" << std::setw(15) << "Size"
                << "Created At" << std::endl;
      std::cout << std::string(85, '-') << std::endl;
      for (auto &f : msg.json_payload["files"]) {
        std::cout << std::left << std::setw(8) << f["id"].get<int>()
                  << std::setw(42) << f["filename"].get<std::string>()
                  << std::setw(15) << FormatSize(f["filesize"].get<uint64_t>())
                  << f["created_at"].get<std::string>() << std::endl;
      }
    } else {
      std::cout << "[ERR] List failed." << std::endl;
    }
    DeleteStream(sid);
  }

  void Upload(const std::string &local_path) {
    uint32_t sid = next_stream_id_++;
    CreateStream(sid);

    std::thread([this, local_path, sid]() {
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
                      {{"filename", filename}, {"filesize", filesize}})) {
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
        uploaded += read;
        // Optionally show progress every few MBs to avoid spam
      }

      // Send completion signal (empty binary)
      SendPacket(Protocol::Command::UploadData, sid, {}, {});
      
      auto msg_done = WaitNextMessage(sid);
      if (msg_done.header.status == 200) {
        std::cout << "\n[Stream #" << sid << " OK] Upload finished: " << filename << std::endl;
      } else {
        std::cout << "\n[Stream #" << sid << " ERR] Upload error at finalization." << std::endl;
      }

      DeleteStream(sid);
    }).detach();

    std::cout << "[INFO] Upload started in background (Stream #" << sid << ")" << std::endl;
  }

  void Download(const std::string &arg) {
    uint32_t sid = next_stream_id_++;
    if (arg.empty() || !std::all_of(arg.begin(), arg.end(), ::isdigit)) {
      std::cout << "[ERR] Please provide a valid numeric File ID." << std::endl;
      return;
    }

    int file_id = std::stoi(arg);
    CreateStream(sid);

    std::thread([this, file_id, sid]() {
      if (!SendPacket(Protocol::Command::DownloadReq, sid,
                      {{"file_id", file_id}, {"offset", 0}})) {
        DeleteStream(sid);
        return;
      }

      auto msg_init = WaitNextMessage(sid);
      if (msg_init.header.magic == 0 || msg_init.header.status != 200) {
        std::cout << "\n[Stream #" << sid << " ERR] Download rejected by server." << std::endl;
        DeleteStream(sid);
        return;
      }

      uint64_t total_size = msg_init.json_payload.value("total_size", 0);
      std::string filename = msg_init.json_payload.value("filename", "downloaded_file");
      std::cout << "\n[Stream #" << sid << " INFO] Starting download: " << filename << " (" << total_size << " bytes)" << std::endl;

      std::ofstream file(filename, std::ios::binary);
      uint64_t downloaded = 0;

      while (true) {
        auto msg = WaitNextMessage(sid);
        if (msg.header.magic == 0 || msg.header.status != 200) {
          std::cout << "\n[Stream #" << sid << " ERR] Download interrupted." << std::endl;
          break;
        }

        if (!msg.binary_payload.empty()) {
          file.write(msg.binary_payload.data(), msg.binary_payload.size());
          downloaded += msg.binary_payload.size();
        }

        if (msg.json_payload.value("eof", false))
          break;
      }
      std::cout << "\n[Stream #" << sid << " OK] Download finished: " << filename << std::endl;
      DeleteStream(sid);
    }).detach();

    std::cout << "[INFO] Download started in background (Stream #" << sid << ")" << std::endl;
  }

  void Remove(const std::string &arg) {
    if (arg.empty() || !std::all_of(arg.begin(), arg.end(), ::isdigit)) {
      std::cout << "[ERR] Please provide a valid numeric File ID. Use 'ls' to "
                   "see IDs."
                << std::endl;
      return;
    }

    int file_id = std::stoi(arg);
    uint32_t sid = next_stream_id_++;
    CreateStream(sid);
    if (!SendPacket(Protocol::Command::Remove, sid, {{"file_id", file_id}})) {
      DeleteStream(sid);
      return;
    }

    auto msg = WaitNextMessage(sid);
    if (msg.header.magic != 0 && msg.header.status == 200) {
      std::cout << "[OK] File deleted." << std::endl;
    } else {
      std::cout << "[ERR] Delete failed." << std::endl;
    }
    DeleteStream(sid);
  }

private:
  std::string ip_;
  uint16_t port_;
  int sock_ = -1;
  std::atomic<uint32_t> next_stream_id_{1};
  
  std::atomic<bool> running_{false};
  std::thread receiver_thread_;
  std::mutex send_mutex_;
  
  std::map<uint32_t, std::shared_ptr<StreamContext>> streams_;
  std::mutex streams_mutex_;
};

int main() {
  AsyCClient client("127.0.0.1", 8080);
  if (!client.Connect()) {
    std::cerr << "Failed to connect to server." << std::endl;
    return 1;
  }

  std::string line;
  std::cout << "AsyCDisk CLI Client. Type 'help' for commands." << std::endl;
  while (true) {
    std::cout << "asyc> " << std::flush;
    if (!std::getline(std::cin, line))
      break;
    if (line.empty())
      continue;

    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;

    if (cmd == "exit")
      break;
    else if (cmd == "help") {
      std::cout << "Commands: login <user> <pass>, ls, put <path>, get <file>, "
                   "rm <file>, exit"
                << std::endl;
    } else if (cmd == "login") {
      std::string u, p;
      ss >> u >> p;
      client.Login(u, p);
    } else if (cmd == "ls") {
      client.List();
    } else if (cmd == "put") {
      // Get the rest of the line as path
      std::string path;
      std::getline(ss >> std::ws, path);
      // Handle quotes if present
      if (path.size() >= 2 && ((path.front() == '"' && path.back() == '"') ||
                               (path.front() == '\'' && path.back() == '\''))) {
        path = path.substr(1, path.size() - 2);
      }
      client.Upload(path);
    } else if (cmd == "get") {
      std::string file;
      std::getline(ss >> std::ws, file);
      if (file.size() >= 2 && ((file.front() == '"' && file.back() == '"') ||
                               (file.front() == '\'' && file.back() == '\''))) {
        file = file.substr(1, file.size() - 2);
      }
      client.Download(file);
    } else if (cmd == "rm") {
      std::string file;
      std::getline(ss >> std::ws, file);
      if (file.size() >= 2 && ((file.front() == '"' && file.back() == '"') ||
                               (file.front() == '\'' && file.back() == '\''))) {
        file = file.substr(1, file.size() - 2);
      }
      client.Remove(file);
    }
  }

  client.Close();
  return 0;
}
