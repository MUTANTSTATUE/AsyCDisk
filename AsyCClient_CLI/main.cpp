#include "AsyCClient.h"
#include <iostream>
#include <sstream>
#include <string>

#include <vector>

struct DirInfo {
  int id;
  std::string name;
};

int main() {
  AsyCClient client("127.0.0.1", 8080);
  if (!client.Connect()) {
    std::cerr << "Failed to connect to server." << std::endl;
    return 1;
  }

  std::vector<DirInfo> path_stack = {{0, "/"}};
  auto get_path_str = [&]() {
    std::string p = "";
    for (size_t i = 0; i < path_stack.size(); ++i) {
        if (i > 0 && path_stack[i-1].name != "/") p += "/";
        p += path_stack[i].name;
    }
    return p;
  };

  std::string line;
  std::cout << "AsyCDisk CLI Client. Type 'help' for commands." << std::endl;
  while (true) {
    std::cout << "asyc:" << get_path_str() << "> " << std::flush;
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
      std::cout << "Commands: login <user> <pass>, ls [id], cd <name|id|..>, pwd, mkdir <name>, put <path>, get <id>, rm <id>, exit"
                << std::endl;
    } else if (cmd == "login") {
      std::string u, p;
      ss >> u >> p;
      client.Login(u, p);
    } else if (cmd == "pwd") {
      std::cout << get_path_str() << std::endl;
    } else if (cmd == "cd") {
      std::string target;
      ss >> target;
      if (target == "..") {
        if (path_stack.size() > 1) path_stack.pop_back();
      } else if (target == "/" || target == "~") {
        path_stack.clear();
        path_stack.push_back({0, "/"});
      } else {
        // Try to find by name in current dir
        json files = client.List(path_stack.back().id);
        bool found = false;
        for (auto& f : files) {
          if (f["is_dir"].get<int>() == 1 && f["filename"].get<std::string>() == target) {
            path_stack.push_back({f["id"].get<int>(), target});
            found = true;
            break;
          }
        }
        if (!found) {
            // Try to find by ID
            try {
                int target_id = std::stoi(target);
                // This is a bit tricky since we don't know the name easily without a reverse lookup
                // For now, let's just say "cd by ID" sets name to "ID:xxx"
                path_stack.push_back({target_id, "id:" + target});
                found = true;
            } catch (...) {
                std::cout << "[ERR] Directory not found: " << target << std::endl;
            }
        }
      }
    } else if (cmd == "mkdir") {
        std::string name;
        ss >> name;
        client.MakeDir(path_stack.back().id, name, [](bool success, std::string msg) {
            if (!success) std::cout << "[ERR] " << msg << std::endl;
        });
    } else if (cmd == "ls") {
      int pid = path_stack.back().id;
      if (ss >> pid) {
          // If user provided an ID, use it
      }
      json files = client.List(pid);
      std::cout << "------------------------------------------------------------" << std::endl;
      printf("%-6s %-32s %-12s %-6s\n", "ID", "Name", "Size", "Type");
      std::cout << "------------------------------------------------------------" << std::endl;
      for (auto &f : files) {
        std::string name = f["filename"];
        long long size = f["filesize"];
        bool is_dir = f["is_dir"].get<int>() == 1;
        printf("%-6d %-32s %-12s %-6s\n", f["id"].get<int>(), name.c_str(),
               is_dir ? "-" : client.FormatSize(size).c_str(), is_dir ? "DIR" : "FILE");
      }
      std::cout << "------------------------------------------------------------" << std::endl;
    } else if (cmd == "put") {
      std::string path;
      std::getline(ss >> std::ws, path);
      if (path.size() >= 2 && ((path.front() == '"' && path.back() == '"') ||
                               (path.front() == '\'' && path.back() == '\''))) {
        path = path.substr(1, path.size() - 2);
      }
      client.Upload(path, path_stack.back().id, [&](uint32_t sid, uint64_t cur, uint64_t total) {
          client.ShowProgressBar(cur, total);
      });
    } else if (cmd == "get") {
      std::string file;
      std::getline(ss >> std::ws, file);
      if (file.size() >= 2 && ((file.front() == '"' && file.back() == '"') ||
                               (file.front() == '\'' && file.back() == '\''))) {
        file = file.substr(1, file.size() - 2);
      }
      try {
          client.Download(std::stoi(file), "", [&](uint32_t sid, uint64_t cur, uint64_t total) {
              client.ShowProgressBar(cur, total);
          });
      } catch (...) {
          std::cout << "Invalid file ID." << std::endl;
      }
    } else if (cmd == "rm") {
      std::string file;
      std::getline(ss >> std::ws, file);
      if (file.size() >= 2 && ((file.front() == '"' && file.back() == '"') ||
                               (file.front() == '\'' && file.back() == '\''))) {
        file = file.substr(1, file.size() - 2);
      }
      try {
          client.Remove(std::stoi(file), [](bool success, std::string msg) {
              std::cout << (success ? "[OK] " : "[ERR] ") << msg << std::endl;
          });
      } catch (...) {
          std::cout << "Invalid file ID." << std::endl;
      }
    }
  }

  client.Close();
  return 0;
}
