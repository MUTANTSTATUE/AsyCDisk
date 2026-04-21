#include "AsyCClient.h"
#include <iostream>
#include <sstream>
#include <string>

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
