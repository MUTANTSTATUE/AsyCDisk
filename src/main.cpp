#include "EventLoop.h"
#include "Logger.h"
#include "TcpServer.h"
#include "Session.h"
#include <iostream>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <uv.h>

int main() {
  Logger::Init();
  LOG_INFO("AsyCDisk Server Starting...");
  // Test sqlite3
  LOG_INFO("SQLite3 version: {}", sqlite3_libversion());
  // Test nlohmann json
  nlohmann::json j = {{"status", "ok"}, {"version", 1.0}};
  LOG_INFO("JSON check: {}", j.dump());
  // Test libuv via EventLoop and TcpServer
  EventLoop loop;
  TcpServer server(&loop, "0.0.0.0", 8080);
  
  std::unordered_set<std::shared_ptr<Session>> active_sessions;
  
  server.SetNewConnectionCallback([&loop, &active_sessions](uv_stream_t* server_stream, int status) {
      if (status < 0) {
          LOG_ERROR("New connection error: {}", uv_strerror(status));
          return;
      }
      
      auto session = std::make_shared<Session>(loop.GetLoop());
      
      if (uv_accept(server_stream, (uv_stream_t*)session->GetSocket()) == 0) {
          LOG_INFO("Client connected!");
          active_sessions.insert(session);
          
          session->SetCloseCallback([&active_sessions](std::shared_ptr<Session> closed_session) {
              active_sessions.erase(closed_session);
              LOG_INFO("Session removed from active list.");
          });
          
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