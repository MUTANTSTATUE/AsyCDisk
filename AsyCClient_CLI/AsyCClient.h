#pragma once

#include "Protocol.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct StreamContext {
  std::queue<Protocol::Message> messages;
  std::mutex mtx;
  std::condition_variable cv;
  bool closed = false;
};

class AsyCClient {
public:
  AsyCClient(const std::string &ip, uint16_t port);
  ~AsyCClient();

  bool Connect();
  void Close();

  void Login(const std::string &user, const std::string &pass);
  void List();
  void Upload(const std::string &local_path);
  void Download(const std::string &arg);
  void Remove(const std::string &arg);

private:
  void ShowProgressBar(uint64_t current, uint64_t total);
  std::string FormatSize(uint64_t bytes);
  
  bool SendPacket(Protocol::Command cmd, uint32_t stream_id,
                  const json &j_payload,
                  const std::vector<char> &b_payload = {});
                  
  bool RecvPacket(Protocol::Message &msg);
  void ReceiverLoop();
  Protocol::Message WaitNextMessage(uint32_t stream_id);
  
  void CreateStream(uint32_t sid);
  void DeleteStream(uint32_t sid);

  std::string ip_;
  uint16_t port_;
  int sock_ = -1;
  std::atomic<uint32_t> next_stream_id_{1};
  
  std::atomic<bool> running_{false};
  std::thread receiver_thread_;
  std::mutex send_mutex_;
  
  std::map<uint32_t, std::shared_ptr<StreamContext>> streams_;
  std::mutex streams_mutex_;
};
