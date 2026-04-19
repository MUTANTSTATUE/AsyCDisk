#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <uv.h>
#include "Logger.h"
#include "EventLoop.h"

int main() {
  Logger::Init();
  LOG_INFO("AsyCDisk Server Starting...");

  // Test libuv via EventLoop
  EventLoop loop;

  // Test sqlite3
  LOG_INFO("SQLite3 version: {}", sqlite3_libversion());

  // Test nlohmann json
  nlohmann::json j = {{"status", "ok"}, {"version", 1.0}};
  LOG_INFO("JSON check: {}", j.dump());

  loop.Run();

  return 0;
}
