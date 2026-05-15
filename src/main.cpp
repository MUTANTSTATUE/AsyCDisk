#include "Config.h"
#include "Database.h"
#include "EventLoop.h"
#include "Logger.h"
#include "Session.h"
#include "TcpServer.h"
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <string>
#include <uv.h>

int main() {
  Logger::Init();
  LOG_INFO("AsyCDisk Server Starting...");

  // 加载配置文件 (必须在这里！)
  if (!Config::GetInstance().Load("config.json")) {
    LOG_ERROR(
        "CRITICAL: Failed to load config.json! Check if file exists in CWD.");
  } else {
    // 必须在任何 libuv 函数调用前设置环境变量，否则不会生效
    int tp_size = Config::GetInstance().Get<int>("libuv/threadpool_size", 4);
    bool use_io_uring =
        Config::GetInstance().Get<bool>("libuv/use_io_uring", true);

    setenv("UV_THREADPOOL_SIZE", std::to_string(tp_size).c_str(), 1);
    setenv("UV_USE_IO_URING", use_io_uring ? "1" : "0", 1);

    LOG_INFO(
        "Config loaded. ThreadPool: {}, io_uring: {}, Upload limit: {} KB/s",
        tp_size, use_io_uring,
        Config::GetInstance().Get<int>("limits/upload_kbps", -1));
  }

  std::string db_path =
      Config::GetInstance().Get<std::string>("storage/db_path", "asycdisk.db");
  std::string data_dir =
      Config::GetInstance().Get<std::string>("storage/data_dir", "data");
  int port = Config::GetInstance().Get<int>("server/port", 8080);

  // Initialize Database
  if (!Database::GetInstance().Open(db_path.c_str())) {
    LOG_CRITICAL("Failed to open database. Exiting.");
    return 1;
  }

  // Ensure data directory exists
  uv_fs_t mkdir_req;
  uv_fs_mkdir(nullptr, &mkdir_req, data_dir.c_str(), 0755, nullptr);
  uv_fs_req_cleanup(&mkdir_req);
  // Test sqlite3
  LOG_INFO("SQLite3 version: {}", sqlite3_libversion());
  // Test nlohmann json
  nlohmann::json j = {{"status", "ok"}, {"version", 1.0}};
  LOG_INFO("JSON check: {}", j.dump());
  // Test libuv via EventLoop and TcpServer
  EventLoop loop;
  TcpServer server(&loop, "0.0.0.0", port);

  server.SetNewConnectionCallback([&loop](uv_stream_t *server_stream,
                                          int status) {
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