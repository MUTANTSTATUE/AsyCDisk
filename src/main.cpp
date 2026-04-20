#include "EventLoop.h"
#include "Logger.h"
#include "TcpServer.h"
#include "Session.h"
#include "Database.h"
#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <uv.h>
int main() {
  Logger::Init();
  LOG_INFO("AsyCDisk Server Starting...");

  // Initialize Database
  if (!Database::GetInstance().Open("asycdisk.db")) {
      LOG_CRITICAL("Failed to open database. Exiting.");
      return 1;
  }

  // Ensure data directory exists
  uv_fs_t mkdir_req;
  uv_fs_mkdir(nullptr, &mkdir_req, "data", 0755, nullptr);
  uv_fs_req_cleanup(&mkdir_req);
  // Test sqlite3
  LOG_INFO("SQLite3 version: {}", sqlite3_libversion());
  // Test nlohmann json
  nlohmann::json j = {{"status", "ok"}, {"version", 1.0}};
  LOG_INFO("JSON check: {}", j.dump());
  // Test libuv via EventLoop and TcpServer
  EventLoop loop;
  TcpServer server(&loop, "0.0.0.0", 8080);

  server.SetNewConnectionCallback(
      [&loop](uv_stream_t *server_stream, int status) {
        if (status < 0) {
          LOG_ERROR("New connection error: {}", uv_strerror(status));
          return;
        }

        auto session = std::make_shared<Session>(loop.GetLoop());

        if (uv_accept(server_stream, (uv_stream_t *)session->GetSocket()) == 0) {
          LOG_INFO("Client connected!");
          session->Start();
        } else {
          session->Close();
        }
      });
  if (!server.Start()) {
    return 1;
  }
  loop.Run();
  return 0;
}