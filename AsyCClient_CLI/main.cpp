#include "Protocol.h"
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iomanip>

using json = nlohmann::json;

class AsyCClient {
public:
    AsyCClient(const std::string& ip, uint16_t port) : ip_(ip), port_(port) {}

    bool Connect() {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

        if (connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            return false;
        }
        return true;
    }

    void Close() {
        if (sock_ >= 0) close(sock_);
    }

    bool SendPacket(Protocol::Command cmd, const json& j_payload, const std::vector<char>& b_payload = {}) {
        std::string j_str;
        if (!j_payload.is_null() && !j_payload.empty()) {
            j_str = j_payload.dump();
        }

        Protocol::Header header;
        header.magic = Protocol::MAGIC_NUMBER;
        header.version = Protocol::CURRENT_VERSION;
        header.command = static_cast<uint16_t>(cmd);
        header.status = 0;
        header.json_len = j_str.size();
        header.binary_len = b_payload.size();

        if (send(sock_, &header, sizeof(header), 0) != sizeof(header)) return false;
        if (header.json_len > 0) {
            if (send(sock_, j_str.data(), j_str.size(), 0) != (ssize_t)j_str.size()) return false;
        }
        if (header.binary_len > 0) {
            if (send(sock_, b_payload.data(), b_payload.size(), 0) != (ssize_t)b_payload.size()) return false;
        }
        return true;
    }

    bool RecvPacket(Protocol::Header& header, json& j_payload, std::vector<char>& b_payload) {
        if (recv(sock_, &header, sizeof(header), MSG_WAITALL) != sizeof(header)) return false;
        if (header.magic != Protocol::MAGIC_NUMBER) return false;

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

    void Login(const std::string& user, const std::string& pass) {
        if (!SendPacket(Protocol::Command::Login, {{"username", user}, {"password", pass}})) return;
        Protocol::Header h; json j; std::vector<char> b;
        if (RecvPacket(h, j, b) && h.status == 200) {
            std::cout << "[OK] Login successful. UserID: " << j["user_id"] << std::endl;
        } else {
            std::cout << "[ERR] Login failed: " << j.value("msg", "unknown error") << std::endl;
        }
    }

    void List() {
        if (!SendPacket(Protocol::Command::ListDir, {{"parent_id", 0}})) return;
        Protocol::Header h; json j; std::vector<char> b;
        if (RecvPacket(h, j, b) && h.status == 200) {
            std::cout << std::left << std::setw(20) << "Filename" << std::setw(15) << "Size" << "Created At" << std::endl;
            std::cout << std::string(50, '-') << std::endl;
            for (auto& f : j["files"]) {
                std::cout << std::left << std::setw(20) << f["filename"].get<std::string>() 
                          << std::setw(15) << f["filesize"] 
                          << f["created_at"].get<std::string>() << std::endl;
            }
        }
    }

    void Upload(const std::string& local_path) {
        std::ifstream file(local_path, std::ios::binary);
        if (!file) { std::cout << "[ERR] Cannot open local file." << std::endl; return; }

        std::string filename = local_path.substr(local_path.find_last_of("/\\") + 1);
        file.seekg(0, std::ios::end);
        size_t filesize = file.tellg();
        file.seekg(0, std::ios::beg);

        if (!SendPacket(Protocol::Command::UploadReq, {{"filename", filename}, {"filesize", filesize}})) return;
        Protocol::Header h; json j; std::vector<char> b;
        if (!RecvPacket(h, j, b) || h.status != 200) {
            std::cout << "[ERR] Upload request rejected: " << j.value("msg", "unknown") << std::endl;
            return;
        }

        size_t offset = j["offset"];
        std::cout << "[INFO] Resuming upload from offset: " << offset << std::endl;
        file.seekg(offset);

        char buf[65536];
        while (!file.eof()) {
            file.read(buf, sizeof(buf));
            size_t read = file.gcount();
            if (read > 0) {
                std::vector<char> chunk(buf, buf + read);
                if (!SendPacket(Protocol::Command::UploadData, {}, chunk)) break;
            }
        }
        // Send completion signal (empty binary)
        SendPacket(Protocol::Command::UploadData, {});
        if (RecvPacket(h, j, b) && h.status == 200) {
            std::cout << "[OK] Upload finished." << std::endl;
        }
    }

    void Download(const std::string& filename) {
        if (!SendPacket(Protocol::Command::DownloadReq, {{"filename", filename}, {"offset", 0}})) return;
        
        std::ofstream file(filename, std::ios::binary);
        while (true) {
            Protocol::Header h; json j; std::vector<char> b;
            if (!RecvPacket(h, j, b)) break;
            if (h.status != 200) { std::cout << "[ERR] Download error." << std::endl; break; }
            
            if (b.size() > 0) file.write(b.data(), b.size());
            if (j.value("eof", false)) break;
        }
        std::cout << "[OK] Download finished: " << filename << std::endl;
    }

    void Remove(const std::string& filename) {
        if (!SendPacket(Protocol::Command::Remove, {{"filename", filename}})) return;
        Protocol::Header h; json j; std::vector<char> b;
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
};

int main() {
    AsyCClient client("127.0.0.1", 8080);
    if (!client.Connect()) {
        std::cerr << "Failed to connect to server." << std::endl;
        return 1;
    }

    std::string cmd;
    std::cout << "AsyCDisk CLI Client. Type 'help' for commands." << std::endl;
    while (true) {
        std::cout << "asyc> ";
        std::cin >> cmd;
        if (cmd == "exit") break;
        else if (cmd == "help") {
            std::cout << "Commands: login <user> <pass>, ls, put <path>, get <file>, rm <file>, exit" << std::endl;
        } else if (cmd == "login") {
            std::string u, p; std::cin >> u >> p;
            client.Login(u, p);
        } else if (cmd == "ls") {
            client.List();
        } else if (cmd == "put") {
            std::string path; std::cin >> path;
            client.Upload(path);
        } else if (cmd == "get") {
            std::string file; std::cin >> file;
            client.Download(file);
        } else if (cmd == "rm") {
            std::string file; std::cin >> file;
            client.Remove(file);
        }
    }

    client.Close();
    return 0;
}
