#pragma once
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "analysis/analysis.h"
#include "analysis/hlsl_check.h"
#include "index/index.h"
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
    std::vector<std::string> variant; // keywords chosen for this document with a code action
    std::vector<std::shared_ptr<const Analysis>> includers; // an include file's, as it was analyzed with them
  };
  void dispatch(const nlohmann::json &message);
  nlohmann::json initialize(const nlohmann::json &params);
  void applySettings(const nlohmann::json &settings);
  // The edits that format the document, or only its lines [first, last] when given. Throws RequestRefused when the
  // file can't be formatted.
  nlohmann::json formatEdits(const Analysis &analysis, std::optional<std::pair<size_t, size_t>> lines);
  // The files around `analysis` as the editor has them, for references and renames.
  // `wait`: give the index time to finish its first crawl, for a question about the whole workspace.
  Workspace workspaceFor(bool wait);
  // For an include file: its includers' files, from the index, which declare what it uses without including it.
  std::vector<std::shared_ptr<const CachedFile>> outerContext(const std::shared_ptr<const Analysis> &analysis);
  // Asks the client to report changes to shader files, when it can.
  void watchFiles();
  void request(const std::string &method, nlohmann::json params);
  void openOrChange(const std::string &uri, int version, std::string text);
  // Analyzes an open document again as it is, after its variant or the settings changed.
  void reanalyze(const std::string &uri);
  AnalyzeOptions analyzeOptions(const std::vector<std::string> &variant);
  // The programs that include an include file directly, from the index, which its #if directives are worked out in.
  std::vector<std::shared_ptr<const Analysis>> includerAnalyses(const std::filesystem::path &path);
  // Analyzes the open include files again whose includers changed.
  void includersChanged();
  nlohmann::json codeActions(const Analysis &analysis, const std::string &uri, const nlohmann::json &params);
  void executeCommand(const nlohmann::json &params);
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
  std::vector<std::filesystem::path> roots_; // workspace folders
  struct {
    bool shaderLab = true;
    bool hlsl = true; // .hlsl, .cginc, .hlslinc, .compute
    bool glsl = true;
  } semanticTokens_;
  std::chrono::milliseconds debounce_{400};
  std::mutex documentsMutex_;
  std::map<std::string, Document> documents_;
  std::mutex workerMutex_;
  std::condition_variable workerWake_;
  std::map<std::string, std::chrono::steady_clock::time_point> pending_;
  bool stopping_ = false;
  std::thread worker_;
  bool watching_ = false; // the client reports file changes
  std::optional<std::filesystem::path> indexCache_; // the indexCache setting: a folder, or empty for none
  int nextRequest_ = 1;
  std::mutex outerMutex_;
  struct Outer {
    std::vector<std::shared_ptr<const Analysis>> includers; // the versions it was worked out from
    std::vector<std::shared_ptr<const CachedFile>> files;
  };
  std::map<std::string, Outer> outerCache_;
  // Last, so that it is destroyed first: its thread calls back into the server.
  Index index_;
};
} // namespace sls
