#include "Protocol.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;

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
    return true;
  }

  void Close() {
    if (sock_ >= 0)
      close(sock_);
  }

  bool SendPacket(Protocol::Command cmd, const json &j_payload,
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
    header.stream_id = current_stream_id_;
    header.json_len = j_str.size();
    header.binary_len = b_payload.size();

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

  bool RecvPacket(Protocol::Header &header, json &j_payload,
                  std::vector<char> &b_payload) {
    if (recv(sock_, &header, sizeof(header), MSG_WAITALL) != sizeof(header))
      return false;
    if (header.magic != Protocol::MAGIC_NUMBER)
      return false;

    // Check if the packet is for our current stream (for now we only have one)
    // In the future, this will be handled by a message dispatcher.
    
    if (header.json_len > 0) {
      std::string j_str(header.json_len, 0);
      recv(sock_, &j_str[0], header.json_len, MSG_WAITALL);
      j_payload = json::parse(j_str);
    }
    if (header.binary_len > 0) {
      b_payload.resize(header.binary_len);
      recv(sock_, &b_payload[0], header.binary_len, MSG_WAITALL);
    }
    return true;
  }

  void Login(const std::string &user, const std::string &pass) {
    current_stream_id_++;
    if (!SendPacket(Protocol::Command::Login,
                    {{"username", user}, {"password", pass}}))
      return;
    Protocol::Header h;
    json j;
    std::vector<char> b;
    if (RecvPacket(h, j, b) && h.status == 200) {
      std::cout << "[OK] Login successful. UserID: " << j["user_id"]
                << std::endl;
    } else {
      std::cout << "[ERR] Login failed: " << j.value("msg", "unknown error")
                << std::endl;
    }
  }

  void List() {
    current_stream_id_++;
    if (!SendPacket(Protocol::Command::ListDir, {{"parent_id", 0}}))
      return;
    Protocol::Header h;
    json j;
    std::vector<char> b;
    if (RecvPacket(h, j, b) && h.status == 200) {
      std::cout << std::left << std::setw(8) << "ID" << std::setw(42)
                << "Filename" << std::setw(15) << "Size"
                << "Created At" << std::endl;
      std::cout << std::string(85, '-') << std::endl;
      for (auto &f : j["files"]) {
        std::cout << std::left << std::setw(8) << f["id"].get<int>()
                  << std::setw(42) << f["filename"].get<std::string>()
                  << std::setw(15) << FormatSize(f["filesize"].get<uint64_t>())
                  << f["created_at"].get<std::string>() << std::endl;
      }
    }
  }

  void Upload(const std::string &local_path) {
    current_stream_id_++;
    std::ifstream file(local_path, std::ios::binary);
    if (!file) {
      std::cout << "[ERR] Cannot open local file." << std::endl;
      return;
    }

    std::string filename =
        local_path.substr(local_path.find_last_of("/\\") + 1);
    file.seekg(0, std::ios::end);
    size_t filesize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (!SendPacket(Protocol::Command::UploadReq,
                    {{"filename", filename}, {"filesize", filesize}}))
      return;
    Protocol::Header h;
    json j;
    std::vector<char> b;
    if (!RecvPacket(h, j, b) || h.status != 200) {
      std::cout << "[ERR] Upload request rejected: "
                << j.value("msg", "unknown") << std::endl;
      return;
    }

    size_t offset = j["offset"];
    std::cout << "[INFO] Resuming upload from offset: " << offset << std::endl;
    file.seekg(offset);

    uint64_t uploaded = offset;
    ShowProgressBar(uploaded, filesize);

    char buf[65536];
    while (!file.eof()) {
      file.read(buf, sizeof(buf));
      size_t read = file.gcount();
      if (read > 0) {
        std::vector<char> chunk(buf, buf + read);
        if (!SendPacket(Protocol::Command::UploadData, {}, chunk))
          break;
        uploaded += read;
        ShowProgressBar(uploaded, filesize);
      }
    }
    // Send completion signal (empty binary)
    SendPacket(Protocol::Command::UploadData, {});
    if (RecvPacket(h, j, b) && h.status == 200) {
      std::cout << "[OK] Upload finished." << std::endl;
    }
  }

  void Download(const std::string &arg) {
    current_stream_id_++;
    if (arg.empty() || !std::all_of(arg.begin(), arg.end(), ::isdigit)) {
      std::cout << "[ERR] Please provide a valid numeric File ID. Use 'ls' to "
                   "see IDs."
                << std::endl;
      return;
    }

    int file_id = std::stoi(arg);
    if (!SendPacket(Protocol::Command::DownloadReq,
                    {{"file_id", file_id}, {"offset", 0}}))
      return;

    Protocol::Header h_init;
    json j_init;
    std::vector<char> b_init;
    if (!RecvPacket(h_init, j_init, b_init) || h_init.status != 200) {
      std::cout << "[ERR] Download rejected." << std::endl;
      return;
    }
    uint64_t total_size = j_init.value("total_size", 0);
    std::string filename = j_init.value("filename", "downloaded_file");
    std::cout << "[INFO] Starting download: " << filename << " (" << total_size
              << " bytes)" << std::endl;

    std::ofstream file(filename, std::ios::binary);
    uint64_t downloaded = 0;
    ShowProgressBar(downloaded, total_size);

    while (true) {
      Protocol::Header h;
      json j;
      std::vector<char> b;
      if (!RecvPacket(h, j, b))
        break;
      if (h.status != 200) {
        std::cout << "[ERR] Download error." << std::endl;
        break;
      }

      if (b.size() > 0) {
        file.write(b.data(), b.size());
        downloaded += b.size();
        ShowProgressBar(downloaded, total_size);
      }
      if (j.value("eof", false))
        break;
    }
    std::cout << "[OK] Download finished: " << filename << std::endl;
  }

  void Remove(const std::string &arg) {
    current_stream_id_++;
    if (arg.empty() || !std::all_of(arg.begin(), arg.end(), ::isdigit)) {
      std::cout << "[ERR] Please provide a valid numeric File ID. Use 'ls' to "
                   "see IDs."
                << std::endl;
      return;
    }

    int file_id = std::stoi(arg);
    if (!SendPacket(Protocol::Command::Remove, {{"file_id", file_id}}))
      return;
    Protocol::Header h;
    json j;
    std::vector<char> b;
    if (RecvPacket(h, j, b) && h.status == 200) {
      std::cout << "[OK] File deleted." << std::endl;
    } else {
      std::cout << "[ERR] Delete failed." << std::endl;
    }
  }

private:
  std::string ip_;
  uint16_t port_;
  int sock_ = -1;
  uint32_t current_stream_id_ = 1;
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
