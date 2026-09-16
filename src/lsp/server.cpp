#include "lsp/server.h"

#include <cstdio>

#include "common/util.h"
#include "format/formatter.h"
#include "fxc/fxc.h"

using json = nlohmann::json;

namespace sls {

namespace {

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kServerNotInitialized = -32002;
constexpr int kRequestFailed = -32803;

json diagnosticJson(const Analysis& analysis, const Diagnostic& diagnostic, Encoding encoding) {
  Span span = diagnostic.span;
  span.begin = std::min(span.begin, analysis.text.size());
  span.end = std::min(std::max(span.end, span.begin), analysis.text.size());
  json result{{"range", toRange(analysis.text, analysis.lines, span, encoding)},
              {"severity", static_cast<int>(diagnostic.severity)},
              {"source", diagnostic.source},
              {"message", diagnostic.message}};
  if (!diagnostic.code.empty()) result["code"] = diagnostic.code;
  if (diagnostic.related) {
    const RelatedLocation& related = *diagnostic.related;
    json position{{"line", std::max(related.line, 0)}, {"character", std::max(related.column, 0)}};
    result["relatedInformation"] = json::array(
        {{{"location", {{"uri", pathToUri(std::filesystem::path(std::u8string(related.path.begin(), related.path.end())))},
                        {"range", {{"start", position}, {"end", position}}}}},
          {"message", related.message}}});
  }
  return result;
}

}  // namespace

Server::Server(Transport& transport) : transport_(transport) {
  worker_ = std::thread([this] { workerLoop(); });
}

Server::~Server() {
  {
    std::lock_guard lock(workerMutex_);
    stopping_ = true;
  }
  workerWake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

int Server::run() {
  while (!exit_) {
    auto body = transport_.read();
    if (!body) break;
    json message;
    try {
      message = json::parse(*body);
    } catch (const std::exception& e) {
      transport_.write({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", kParseError}, {"message", e.what()}}}});
      continue;
    }
    try {
      dispatch(message);
    } catch (const std::exception& e) {
      if (message.contains("id")) respondError(message["id"], kRequestFailed, e.what());
      std::fprintf(stderr, "shaderlab-ls: %s\n", e.what());
    }
  }
  return shutdownRequested_ ? 0 : 1;
}

void Server::respond(const json& id, json result) {
  transport_.write({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void Server::respondError(const json& id, int code, const std::string& message) {
  transport_.write({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

void Server::notify(const std::string& method, json params) {
  transport_.write({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

std::shared_ptr<const Analysis> Server::snapshot(const std::string& uri) {
  std::lock_guard lock(documentsMutex_);
  auto it = documents_.find(uri);
  return it == documents_.end() ? nullptr : it->second.analysis;
}

void Server::dispatch(const json& message) {
  if (!message.is_object() || !message.contains("method")) return;  // responses to our requests are ignored
  const std::string method = message["method"].get<std::string>();
  const bool isRequest = message.contains("id");
  const json id = isRequest ? message["id"] : json();
  const json params = message.value("params", json::object());

  if (method == "initialize") {
    respond(id, initialize(params));
    initialized_ = true;
    return;
  }
  if (method == "exit") {
    exit_ = true;
    return;
  }
  if (!initialized_) {
    if (isRequest) respondError(id, kServerNotInitialized, "Server not initialized");
    return;
  }
  if (method == "shutdown") {
    shutdownRequested_ = true;
    respond(id, nullptr);
    return;
  }
  if (shutdownRequested_ && isRequest) {
    respondError(id, kInvalidRequest, "Server is shutting down");
    return;
  }

  if (method == "textDocument/didOpen") {
    const json& doc = params.at("textDocument");
    openOrChange(doc.at("uri").get<std::string>(), doc.value("version", 0), doc.at("text").get<std::string>());
    return;
  }
  if (method == "textDocument/didChange") {
    const json& doc = params.at("textDocument");
    const json& changes = params.at("contentChanges");
    if (changes.empty()) return;
    // Full synchronization: the last change carries the whole text.
    openOrChange(doc.at("uri").get<std::string>(), doc.value("version", 0), changes.back().at("text").get<std::string>());
    return;
  }
  if (method == "textDocument/didSave") {
    schedule(params.at("textDocument").at("uri").get<std::string>(), std::chrono::milliseconds(0));
    return;
  }
  if (method == "textDocument/didClose") {
    std::string uri = params.at("textDocument").at("uri").get<std::string>();
    {
      std::lock_guard lock(documentsMutex_);
      documents_.erase(uri);
    }
    notify("textDocument/publishDiagnostics", {{"uri", uri}, {"diagnostics", json::array()}});
    return;
  }
  if (method == "workspace/didChangeConfiguration") {
    json settings = params.value("settings", json::object());
    if (settings.contains("shaderlab")) settings = settings["shaderlab"];
    if (settings.is_object()) applySettings(settings);
    std::vector<std::string> uris;
    {
      std::lock_guard lock(documentsMutex_);
      for (const auto& [uri, doc] : documents_) uris.push_back(uri);
    }
    for (const std::string& uri : uris) schedule(uri, std::chrono::milliseconds(0));
    return;
  }

  if (method == "textDocument/formatting") {
    auto analysis = snapshot(params.at("textDocument").at("uri").get<std::string>());
    if (!analysis || analysis->kind != DocumentKind::ShaderLab) {
      respond(id, nullptr);
      return;
    }
    Encoding encoding;
    {
      std::lock_guard lock(settingsMutex_);
      encoding = context_.encoding;
    }
    const json options = params.value("options", json::object());
    FormatOptions formatOptions;
    formatOptions.indentSize = options.value("tabSize", 4);
    formatOptions.useTabs = !options.value("insertSpaces", true);
    FormatResult result = formatShaderLab(analysis->text, formatOptions);
    if (!result.ok) {
      respondError(id, kRequestFailed, "shaderlab-ls can't format this file: " + result.error);
      return;
    }
    if (result.text == analysis->text) {
      respond(id, json::array());
      return;
    }
    respond(id, json::array({{{"range", toRange(analysis->text, analysis->lines, {0, analysis->text.size()}, encoding)},
                              {"newText", std::move(result.text)}}}));
    return;
  }

  if (method == "textDocument/completion" || method == "textDocument/hover" || method == "textDocument/definition" ||
      method == "textDocument/documentSymbol") {
    std::string uri = params.at("textDocument").at("uri").get<std::string>();
    auto analysis = snapshot(uri);
    if (!analysis) {
      respond(id, nullptr);
      return;
    }
    FeatureContext context;
    {
      std::lock_guard lock(settingsMutex_);
      context = context_;
    }
    if (method == "textDocument/documentSymbol") {
      respond(id, documentSymbols(*analysis, context));
      return;
    }
    const json& position = params.at("position");
    size_t offset = analysis->lines.toOffset(
        analysis->text, {position.at("line").get<int>(), position.at("character").get<int>()}, context.encoding);
    if (method == "textDocument/completion") respond(id, completion(*analysis, offset, context));
    if (method == "textDocument/hover") respond(id, hover(*analysis, offset, context));
    if (method == "textDocument/definition") respond(id, definition(*analysis, offset, context));
    return;
  }

  if (isRequest) respondError(id, kMethodNotFound, "Method not found: " + method);
}

json Server::initialize(const json& params) {
  Encoding encoding = Encoding::Utf16;
  const json capabilities = params.value("capabilities", json::object());
  if (capabilities.contains("general") && capabilities["general"].contains("positionEncodings")) {
    for (const json& value : capabilities["general"]["positionEncodings"]) {
      if (value == "utf-8") encoding = Encoding::Utf8;
    }
  }
  bool snippets = false;
  try {
    snippets = capabilities.at("textDocument").at("completion").at("completionItem").value("snippetSupport", false);
  } catch (...) {
  }
  {
    std::lock_guard lock(settingsMutex_);
    context_.encoding = encoding;
    context_.snippets = snippets;
  }
  if (params.contains("initializationOptions") && params["initializationOptions"].is_object()) {
    applySettings(params["initializationOptions"]);
  }

  const Fxc& fxc = Fxc::instance();
  if (!fxc.available()) {
    notify("window/logMessage", {{"type", 2}, {"message", fxc.loadError()}});
  }

  return {
      {"capabilities",
       {
           {"positionEncoding", encoding == Encoding::Utf8 ? "utf-8" : "utf-16"},
           {"textDocumentSync", {{"openClose", true}, {"change", 1}, {"save", {{"includeText", false}}}}},
           {"completionProvider", {{"triggerCharacters", {"[", "\"", "#"}}}},
           {"hoverProvider", true},
           {"definitionProvider", true},
           {"documentSymbolProvider", true},
           {"documentFormattingProvider", true},
       }},
      {"serverInfo", {{"name", "shaderlab-ls"}, {"version", SHADERLAB_LS_VERSION}}},
  };
}

// {
//   "unityEditorPath": "C:/Program Files/Unity/Hub/Editor/6000.6.0f1/Editor",
//   "keywords": ["_NORMALMAP"],
//   "defines": ["MY_DEFINE", "OTHER=2"],
//   "diagnostics": { "fxc": true, "delay": 400 }
// }
void Server::applySettings(const json& settings) {
  std::lock_guard lock(settingsMutex_);
  if (settings.contains("unityEditorPath") && settings["unityEditorPath"].is_string()) {
    std::string path = settings["unityEditorPath"].get<std::string>();
    context_.editorOverride = std::filesystem::path(std::u8string(path.begin(), path.end()));
    checkOptions_.editorOverride = context_.editorOverride;
  }
  if (settings.contains("keywords") && settings["keywords"].is_array()) {
    checkOptions_.keywords.clear();
    for (const json& keyword : settings["keywords"]) {
      if (keyword.is_string()) checkOptions_.keywords.push_back(keyword.get<std::string>());
    }
  }
  if (settings.contains("defines") && settings["defines"].is_array()) {
    checkOptions_.defines.clear();
    for (const json& define : settings["defines"]) {
      if (!define.is_string()) continue;
      std::string text = define.get<std::string>();
      size_t equals = text.find('=');
      checkOptions_.defines.push_back(equals == std::string::npos ? FxcDefine{text, "1"}
                                                                  : FxcDefine{text.substr(0, equals), text.substr(equals + 1)});
    }
  }
  if (settings.contains("diagnostics") && settings["diagnostics"].is_object()) {
    const json& diagnostics = settings["diagnostics"];
    fxcEnabled_ = diagnostics.value("fxc", fxcEnabled_);
    debounce_ = std::chrono::milliseconds(diagnostics.value("delay", static_cast<int>(debounce_.count())));
  }
}

void Server::openOrChange(const std::string& uri, int version, std::string text) {
  auto analysis = analyze(uriToPath(uri), std::move(text));
  {
    std::lock_guard lock(documentsMutex_);
    Document& doc = documents_[uri];
    doc.version = version;
    doc.analysis = std::move(analysis);
  }
  publish(uri);
  std::chrono::milliseconds delay;
  {
    std::lock_guard lock(settingsMutex_);
    delay = debounce_;
  }
  schedule(uri, delay);
}

void Server::publish(const std::string& uri) {
  Encoding encoding;
  {
    std::lock_guard lock(settingsMutex_);
    encoding = context_.encoding;
  }
  json diagnostics = json::array();
  int version = 0;
  {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    if (it == documents_.end()) return;
    const Analysis& analysis = *it->second.analysis;
    version = it->second.version;
    for (const Diagnostic& diagnostic : analysis.diagnostics) diagnostics.push_back(diagnosticJson(analysis, diagnostic, encoding));
    for (const Diagnostic& diagnostic : it->second.fxc) diagnostics.push_back(diagnosticJson(analysis, diagnostic, encoding));
  }
  notify("textDocument/publishDiagnostics", {{"uri", uri}, {"version", version}, {"diagnostics", std::move(diagnostics)}});
}

void Server::schedule(const std::string& uri, std::chrono::milliseconds delay) {
  {
    std::lock_guard lock(settingsMutex_);
    if (!fxcEnabled_ || !Fxc::instance().available()) return;
  }
  {
    std::lock_guard lock(workerMutex_);
    pending_[uri] = std::chrono::steady_clock::now() + delay;
  }
  workerWake_.notify_all();
}

void Server::workerLoop() {
  std::unique_lock lock(workerMutex_);
  while (!stopping_) {
    if (pending_.empty()) {
      workerWake_.wait(lock);
      continue;
    }
    auto next = std::min_element(pending_.begin(), pending_.end(),
                                 [](const auto& a, const auto& b) { return a.second < b.second; });
    if (std::chrono::steady_clock::now() < next->second) {
      workerWake_.wait_until(lock, next->second);
      continue;
    }
    std::string uri = next->first;
    pending_.erase(next);
    lock.unlock();
    try {
      runCheck(uri);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "shaderlab-ls: FXC check failed: %s\n", e.what());
    }
    lock.lock();
  }
}

void Server::runCheck(const std::string& uri) {
  std::shared_ptr<const Analysis> analysis;
  int version = 0;
  {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    if (it == documents_.end()) return;
    analysis = it->second.analysis;
    version = it->second.version;
  }
  CheckOptions options;
  {
    std::lock_guard lock(settingsMutex_);
    options = checkOptions_;
  }
  std::function<bool()> cancelled = [&] {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    return it == documents_.end() || it->second.analysis != analysis;
  };
  std::vector<Diagnostic> diagnostics = checkHlsl(*analysis, options, cancelled);
  {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    if (it == documents_.end() || it->second.analysis != analysis || it->second.version != version) return;
    it->second.fxc = std::move(diagnostics);
  }
  publish(uri);
}

}  // namespace sls
