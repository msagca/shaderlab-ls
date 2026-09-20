#pragma once
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "analysis/analysis.h"
#include "analysis/hlsl_check.h"
#include "lsp/features.h"
#include "lsp/transport.h"
namespace sls {
class Server {
public:
  explicit Server(Transport &transport);
  ~Server();
  // Runs until "exit". Returns the process exit code.
  int run();
private:
  struct Document {
    int version = 0;
    std::shared_ptr<const Analysis> analysis;
    std::vector<Diagnostic> hlsl; // from the last compile
  };
  void dispatch(const nlohmann::json &message);
  nlohmann::json initialize(const nlohmann::json &params);
  void applySettings(const nlohmann::json &settings);
  void openOrChange(const std::string &uri, int version, std::string text);
  void publish(const std::string &uri);
  void schedule(const std::string &uri, std::chrono::milliseconds delay);
  void workerLoop();
  void runCheck(const std::string &uri);
  std::shared_ptr<const Analysis> snapshot(const std::string &uri);
  void respond(const nlohmann::json &id, nlohmann::json result);
  void respondError(const nlohmann::json &id, int code, const std::string &message);
  void notify(const std::string &method, nlohmann::json params);
  Transport &transport_;
  bool initialized_ = false;
  bool shutdownRequested_ = false;
  bool exit_ = false;
  std::mutex settingsMutex_;
  FeatureContext context_;
  CheckOptions checkOptions_;
  std::string clangFormatPath_; // empty: clang-format from PATH
  std::chrono::milliseconds debounce_{400};
  std::mutex documentsMutex_;
  std::map<std::string, Document> documents_;
  std::mutex workerMutex_;
  std::condition_variable workerWake_;
  std::map<std::string, std::chrono::steady_clock::time_point> pending_;
  bool stopping_ = false;
  std::thread worker_;
};
} // namespace sls
