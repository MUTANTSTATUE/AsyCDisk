#include <iostream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <uv.h>
#include "Logger.h"

int main() {
  Logger::Init();
  LOG_INFO("AsyCDisk Server Starting...");

  // Test libuv
  uv_loop_t *loop = new uv_loop_t();
  uv_loop_init(loop);
  LOG_INFO("libuv loop initialized.");

  // Test sqlite3
  LOG_INFO("SQLite3 version: {}", sqlite3_libversion());

  // Test nlohmann json
  nlohmann::json j = {{"status", "ok"}, {"version", 1.0}};
  LOG_INFO("JSON check: {}", j.dump());

  uv_run(loop, UV_RUN_DEFAULT);
  uv_loop_close(loop);
  free(loop);

  return 0;
}
