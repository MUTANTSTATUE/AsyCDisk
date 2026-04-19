#include <iostream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <uv.h>

int main() {
  spdlog::info("AsyCDisk Server Starting...");

  // Test libuv
  uv_loop_t *loop = new uv_loop_t();
  uv_loop_init(loop);
  spdlog::info("libuv loop initialized.");

  // Test sqlite3
  spdlog::info("SQLite version: {}", sqlite3_libversion());

  // Test nlohmann json
  nlohmann::json j = {{"status", "ok"}, {"version", 1.0}};
  spdlog::info("JSON check: {}", j.dump());

  uv_run(loop, UV_RUN_DEFAULT);
  uv_loop_close(loop);
  free(loop);

  return 0;
}
