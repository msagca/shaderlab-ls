#include "lsp/server.h"
#include <cstdio>
#include "common/util.h"
#include "format/diff.h"
#include "format/formatter.h"
#include "analysis/variant.h"
#include "lsp/semantic_tokens.h"
#include "compiler/dxc.h"
#include "compiler/fxc.h"
#include "unity/project.h"
using json = nlohmann::json;
namespace sls {
namespace {
  constexpr int kParseError = -32700;
  constexpr int kInvalidRequest = -32600;
  constexpr int kMethodNotFound = -32601;
  constexpr int kServerNotInitialized = -32002;
  constexpr int kRequestFailed = -32803;
  // Arguments: the document's URI, then a keyword set and the keyword of it to enable ("_" for none); the URI alone
  // goes back to the default variant.
  constexpr const char *kCheckVariant = "shaderlab-ls.checkVariant";
  json diagnosticJson(const Analysis &analysis, const Diagnostic &diagnostic, Encoding encoding) {
    Span span = diagnostic.span;
    span.begin = std::min(span.begin, analysis.text.size());
    span.end = std::min(std::max(span.end, span.begin), analysis.text.size());
    json result{{"range", toRange(analysis.text, analysis.lines, span, encoding)},
      {"severity", static_cast<int>(diagnostic.severity)},
      {"source", diagnostic.source},
      {"message", diagnostic.message}};
    if (!diagnostic.code.empty())
      result["code"] = diagnostic.code;
    if (diagnostic.unnecessary)
      result["tags"] = json::array({1}); // DiagnosticTag.Unnecessary
    if (diagnostic.related) {
      const RelatedLocation &related = *diagnostic.related;
      json position{{"line", std::max(related.line, 0)}, {"character", std::max(related.column, 0)}};
      result["relatedInformation"] = json::array(
        {{{"location", {{"uri", pathToUri(std::filesystem::path(std::u8string(related.path.begin(), related.path.end())))}, {"range", {{"start", position}, {"end", position}}}}},
          {"message", related.message}}});
    }
    return result;
  }
  // Which HLSL compilers there are before any project is known. A project's Unity editor may still bring a DXC.
  json compilerLog(const CheckOptions &options) {
    const Fxc &fxc = Fxc::instance();
    const Dxc *dxc = nullptr;
    if (!options.dxcLibrary.empty() && Dxc::load(options.dxcLibrary).available()) {
      dxc = &Dxc::load(options.dxcLibrary);
    } else if (Dxc::load({}).available()) {
      dxc = &Dxc::load({});
    }
    std::string found;
    if (fxc.available())
      found = "FXC (" + fxc.libraryPath() + ")";
    if (dxc)
      found += (found.empty() ? "" : ", ") + std::string("DXC (") + dxc->libraryPath() + ")";
    if (!found.empty())
      return {{"type", 3}, {"message", "HLSL compilers: " + found}};
    return {{"type", 2},
      {"message", "No HLSL compiler found: FXC exists on Windows only, and there is no " + std::string(dxcLibraryName()) + " on the library path or at dxcPath. HLSL is compiled only in projects whose Unity editor "
                                                                                                                           "ships DXC."}};
  }
} // namespace
Server::Server(Transport &transport)
  : transport_(transport) {
  worker_ = std::thread([this] { workerLoop(); });
  index_.onLog([this](const std::string &message) { notify("window/logMessage", {{"type", 4}, {"message", message}}); });
  index_.onIdle([this] { includersChanged(); });
}
Server::~Server() {
  {
    std::lock_guard lock(workerMutex_);
    stopping_ = true;
  }
  workerWake_.notify_all();
  if (worker_.joinable())
    worker_.join();
}
int Server::run() {
  while (!exit_) {
    auto body = transport_.read();
    if (!body)
      break;
    json message;
    try {
      message = json::parse(*body);
    } catch (const std::exception &e) {
      transport_.write({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", kParseError}, {"message", e.what()}}}});
      continue;
    }
    try {
      dispatch(message);
    } catch (const RequestRefused &e) {
      if (message.contains("id"))
        respondError(message["id"], kRequestFailed, e.what());
    } catch (const std::exception &e) {
      if (message.contains("id"))
        respondError(message["id"], kRequestFailed, e.what());
      std::fprintf(stderr, "shaderlab-ls: %s\n", e.what());
    }
  }
  return shutdownRequested_ ? 0 : 1;
}
void Server::respond(const json &id, json result) {
  transport_.write({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}
void Server::respondError(const json &id, int code, const std::string &message) {
  transport_.write({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}
void Server::request(const std::string &method, json params) {
  transport_.write({{"jsonrpc", "2.0"}, {"id", "shaderlab-ls-" + std::to_string(nextRequest_++)}, {"method", method}, {"params", std::move(params)}});
}
void Server::notify(const std::string &method, json params) {
  transport_.write({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}
std::shared_ptr<const Analysis> Server::snapshot(const std::string &uri) {
  std::lock_guard lock(documentsMutex_);
  auto it = documents_.find(uri);
  return it == documents_.end() ? nullptr : it->second.analysis;
}
void Server::dispatch(const json &message) {
  if (!message.is_object() || !message.contains("method"))
    return; // responses to our requests are ignored
  const std::string method = message["method"].get<std::string>();
  const bool isRequest = message.contains("id");
  const json id = isRequest ? message["id"] : json();
  const json params = message.value("params", json::object());
  if (method == "initialize") {
    respond(id, initialize(params));
    initialized_ = true;
    return;
  }
  if (method == "initialized") {
    if (watching_)
      watchFiles();
    return;
  }
  if (method == "exit") {
    exit_ = true;
    return;
  }
  if (!initialized_) {
    if (isRequest)
      respondError(id, kServerNotInitialized, "Server not initialized");
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
    const json &doc = params.at("textDocument");
    openOrChange(doc.at("uri").get<std::string>(), doc.value("version", 0), doc.at("text").get<std::string>());
    return;
  }
  if (method == "textDocument/didChange") {
    const json &doc = params.at("textDocument");
    const json &changes = params.at("contentChanges");
    if (changes.empty())
      return;
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
    index_.documentClosed(uriToPath(uri));
    notify("textDocument/publishDiagnostics", {{"uri", uri}, {"diagnostics", json::array()}});
    return;
  }
  if (method == "workspace/didChangeWatchedFiles") {
    constexpr int kDeleted = 3; // FileChangeType
    for (const json &change : params.value("changes", json::array())) {
      std::filesystem::path path = uriToPath(change.value("uri", ""));
      if (change.value("type", 0) == kDeleted)
        index_.fileDeleted(path);
      else
        index_.fileChanged(path);
    }
    return;
  }
  if (method == "workspace/didChangeConfiguration") {
    json settings = params.value("settings", json::object());
    if (settings.contains("shaderlab"))
      settings = settings["shaderlab"];
    if (settings.is_object())
      applySettings(settings);
    std::vector<std::string> uris;
    {
      std::lock_guard lock(documentsMutex_);
      for (const auto &[uri, doc] : documents_)
        uris.push_back(uri);
    }
    // The keywords and defines decide what the #if directives leave out, as well as what is compiled.
    for (const std::string &uri : uris)
      reanalyze(uri);
    {
      std::lock_guard lock(settingsMutex_);
      index_.setRoots(roots_, context_.editorOverride); // the editor decides where package includes resolve
    }
    return;
  }
  if (method == "workspace/symbol") {
    FeatureContext context;
    {
      std::lock_guard lock(settingsMutex_);
      context = context_;
    }
    respond(id, workspaceSymbols(params.value("query", ""), workspaceFor(true), context));
    return;
  }
  if (method == "workspace/executeCommand") {
    executeCommand(params);
    respond(id, nullptr);
    return;
  }
  if (method == "workspace/didChangeWorkspaceFolders") {
    const json event = params.value("event", json::object());
    std::lock_guard lock(settingsMutex_);
    for (const json &folder : event.value("removed", json::array())) {
      std::string key = pathKey(uriToPath(folder.value("uri", "")));
      std::erase_if(roots_, [&](const std::filesystem::path &root) { return pathKey(root) == key; });
    }
    for (const json &folder : event.value("added", json::array()))
      roots_.push_back(uriToPath(folder.value("uri", "")));
    index_.setRoots(roots_, context_.editorOverride);
    return;
  }
  if (method == "textDocument/formatting" || method == "textDocument/rangeFormatting") {
    auto analysis = snapshot(params.at("textDocument").at("uri").get<std::string>());
    if (!analysis) {
      respond(id, nullptr);
      return;
    }
    std::optional<std::pair<size_t, size_t>> lines;
    if (method == "textDocument/rangeFormatting") {
      const json &range = params.at("range");
      size_t first = range.at("start").at("line").get<size_t>();
      size_t last = range.at("end").at("line").get<size_t>();
      // A selection that ends at the start of a line does not take that line in.
      if (last > first && range.at("end").at("character").get<int>() == 0)
        --last;
      lines = {first, last};
    }
    respond(id, formatEdits(*analysis, lines));
    return;
  }
  if (method == "textDocument/completion" || method == "textDocument/hover" || method == "textDocument/definition" ||
      method == "textDocument/documentSymbol" || method == "textDocument/references" ||
      method == "textDocument/documentHighlight" || method == "textDocument/prepareRename" ||
      method == "textDocument/rename" || method == "textDocument/semanticTokens/full" || method == "textDocument/codeAction" ||
      method == "textDocument/signatureHelp") {
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
    context.outer = outerContext(analysis);
    if (method == "textDocument/documentSymbol") {
      respond(id, documentSymbols(*analysis, context));
      return;
    }
    if (method == "textDocument/codeAction") {
      respond(id, codeActions(*analysis, uri, params));
      return;
    }
    if (method == "textDocument/semanticTokens/full") {
      bool wanted;
      {
        std::lock_guard lock(settingsMutex_);
        wanted = analysis->kind == DocumentKind::ShaderLab ? semanticTokens_.shaderLab
                 : analysis->kind == DocumentKind::Glsl    ? semanticTokens_.glsl
                                                           : semanticTokens_.hlsl;
      }
      respond(id, wanted ? semanticTokens(*analysis, context) : json{{"data", json::array()}});
      return;
    }
    const json &position = params.at("position");
    size_t offset = analysis->lines.toOffset(
      analysis->text,
      {position.at("line").get<int>(), position.at("character").get<int>()},
      context.encoding);
    if (method == "textDocument/completion")
      respond(id, completion(*analysis, offset, context));
    if (method == "textDocument/hover")
      respond(id, hover(*analysis, offset, context));
    if (method == "textDocument/signatureHelp")
      respond(id, signatureHelp(*analysis, offset, context));
    if (method == "textDocument/definition")
      respond(id, definition(*analysis, offset, context));
    if (method == "textDocument/references") {
      bool includeDeclaration = params.value("context", json::object()).value("includeDeclaration", true);
      respond(id, references(*analysis, uri, offset, includeDeclaration, context, workspaceFor(true)));
    }
    if (method == "textDocument/documentHighlight")
      respond(id, documentHighlights(*analysis, offset, context));
    if (method == "textDocument/prepareRename")
      respond(id, prepareRename(*analysis, offset, context, workspaceFor(true)));
    if (method == "textDocument/rename")
      respond(id, rename(*analysis, uri, offset, params.at("newName").get<std::string>(), context, workspaceFor(true)));
    return;
  }
  if (isRequest)
    respondError(id, kMethodNotFound, "Method not found: " + method);
}
Workspace Server::workspaceFor(bool wait) {
  std::vector<std::filesystem::path> roots;
  std::filesystem::path editorOverride;
  {
    std::lock_guard lock(settingsMutex_);
    roots = roots_;
    editorOverride = context_.editorOverride;
  }
  std::map<std::string, Workspace::OpenDocument> open;
  {
    std::lock_guard lock(documentsMutex_);
    for (const auto &[uri, document] : documents_)
      open[pathKey(document.analysis->path)] = {uri, document.analysis};
  }
  // A document open from outside every workspace folder, or with no folder at all, still reaches the rest of its
  // Unity project.
  for (const auto &[key, document] : open) {
    if (Workspace(roots, {}).contains(document.analysis->path))
      continue;
    auto project = UnityProject::forFile(document.analysis->path, editorOverride);
    if (!project->root().empty())
      roots.push_back(project->root());
  }
  // A client that reports no file changes has the folders crawled again, at most every few seconds, for changes made
  // outside the editor. The index answers meanwhile with what it has.
  if (!watching_)
    index_.refresh();
  // The first crawl of a large project takes a while; until it is done, the folders are read as they are.
  bool ready = index_.waitReady(wait ? std::chrono::seconds(10) : std::chrono::seconds(0));
  return Workspace(roots, std::move(open), ready ? &index_ : nullptr);
}
std::vector<std::shared_ptr<const CachedFile>> Server::outerContext(const std::shared_ptr<const Analysis> &document) {
  const Analysis &analysis = *document;
  if (analysis.kind != DocumentKind::HlslInclude)
    return {};
  std::string key = pathKey(analysis.path);
  std::filesystem::path editorOverride;
  {
    std::lock_guard lock(settingsMutex_);
    editorOverride = context_.editorOverride;
  }
  // The includers as they are now; the context is worked out again only when one of them changed, not whenever this
  // file does.
  Workspace workspace = workspaceFor(false);
  constexpr size_t kIncluders = 16, kFiles = 400;
  std::vector<std::shared_ptr<const Analysis>> includers;
  for (const std::filesystem::path &path : index_.includers(analysis.path, kIncluders)) {
    if (auto includer = workspace.analysis(path))
      includers.push_back(std::move(includer));
  }
  {
    std::lock_guard lock(outerMutex_);
    auto it = outerCache_.find(key);
    if (it != outerCache_.end() && it->second.includers == includers)
      return it->second.files;
  }
  std::vector<std::shared_ptr<const CachedFile>> files;
  std::set<std::string> seen{key};
  // The nearest includers first, each with what it includes besides this file: what this file's code is compiled
  // with, wherever it is included.
  for (const std::shared_ptr<const Analysis> &includer : includers) {
    if (!seen.insert(pathKey(includer->path)).second)
      continue;
    auto file = std::make_shared<CachedFile>();
    file->path = includer->path;
    file->text = includer->text;
    file->lines = includer->lines;
    std::vector<size_t> code;
    for (size_t u = 0; u < includer->units.size(); ++u) {
      if (includer->isGlsl(u))
        continue;
      code.push_back(u);
      const HlslScan &scan = includer->units[u].scan;
      file->scan.decls.insert(file->scan.decls.end(), scan.decls.begin(), scan.decls.end());
      file->scan.includes.insert(file->scan.includes.end(), scan.includes.begin(), scan.includes.end());
    }
    files.push_back(std::move(file));
    for (auto &included : includedFiles(*includer, code, editorOverride)) {
      if (seen.insert(pathKey(included->path)).second)
        files.push_back(std::move(included));
    }
    if (files.size() >= kFiles)
      break;
  }
  std::lock_guard lock(outerMutex_);
  outerCache_[key] = {std::move(includers), files};
  return files;
}
void Server::watchFiles() {
  json watcher{{"globPattern", "**/*.{shader,hlsl,cginc,hlslinc,compute}"}};
  json registration{{"id", "shaderlab-ls-files"}, {"method", "workspace/didChangeWatchedFiles"}, {"registerOptions", {{"watchers", json::array({watcher})}}}};
  request("client/registerCapability", {{"registrations", json::array({registration})}});
}
std::vector<std::shared_ptr<const Analysis>> Server::includerAnalyses(const std::filesystem::path &path) {
  if (documentKindFor(path) != DocumentKind::HlslInclude)
    return {};
  std::vector<std::filesystem::path> direct = index_.directIncluders(path);
  if (direct.empty())
    return {};
  constexpr size_t kIncluders = 8;
  Workspace workspace = workspaceFor(false);
  std::vector<std::shared_ptr<const Analysis>> result;
  for (const std::filesystem::path &includer : direct) {
    if (result.size() >= kIncluders)
      break;
    if (auto analysis = workspace.analysis(includer))
      result.push_back(std::move(analysis));
  }
  return result;
}
void Server::includersChanged() {
  std::vector<std::pair<std::string, std::vector<std::shared_ptr<const Analysis>>>> includeFiles;
  {
    std::lock_guard lock(documentsMutex_);
    for (const auto &[uri, document] : documents_) {
      if (document.analysis->kind == DocumentKind::HlslInclude)
        includeFiles.emplace_back(uri, document.includers);
    }
  }
  // Only where they did change: analyzing a file tells the index of it, which calls this again when it is done.
  for (const auto &[uri, includers] : includeFiles) {
    if (includerAnalyses(uriToPath(uri)) != includers)
      reanalyze(uri);
  }
}
void Server::reanalyze(const std::string &uri) {
  std::string text;
  int version = 0;
  {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    if (it == documents_.end())
      return;
    text = it->second.analysis->text;
    version = it->second.version;
  }
  openOrChange(uri, version, std::move(text));
}
json Server::codeActions(const Analysis &analysis, const std::string &uri, const json &params) {
  json actions = json::array();
  Encoding encoding;
  std::vector<std::string> global;
  {
    std::lock_guard lock(settingsMutex_);
    encoding = context_.encoding;
    global = checkOptions_.keywords;
  }
  std::vector<std::string> variant;
  {
    std::lock_guard lock(documentsMutex_);
    if (auto it = documents_.find(uri); it != documents_.end())
      variant = it->second.variant;
  }
  const json &range = params.at("range");
  size_t first = range.at("start").at("line").get<size_t>();
  size_t last = range.at("end").at("line").get<size_t>();
  auto command = [&](const std::string &title, json arguments) {
    actions.push_back({{"title", title}, {"kind", "source"}, {"command", {{"title", title}, {"command", kCheckVariant}, {"arguments", std::move(arguments)}}}});
  };
  // On a keyword set's line: compile the variant with another of its keywords.
  for (size_t u = 0; u < analysis.units.size(); ++u) {
    if (analysis.isGlsl(u))
      continue;
    for (const HlslPragma &pragma : analysis.units[u].scan.pragmas) {
      size_t line = analysis.lines.lineOf(pragma.span.begin);
      if (!isVariantPragma(pragma) || line < first || line > last)
        continue;
      std::vector<std::string> enabled = enabledKeywords(pragma, {variant, global});
      json set = json::array();
      std::string keywords;
      for (const PragmaArg &arg : pragma.args) {
        set.push_back(arg.text);
        if (!isNoKeyword(arg.text))
          keywords += (keywords.empty() ? "" : ", ") + arg.text;
      }
      std::set<std::string> offered;
      std::vector<PragmaArg> choices = pragma.args;
      if (allowsNone(pragma))
        choices.push_back({"_", {}});
      for (const PragmaArg &arg : choices) {
        bool none = isNoKeyword(arg.text);
        bool current = none ? enabled.empty() : std::find(enabled.begin(), enabled.end(), arg.text) != enabled.end();
        if (current || !offered.insert(none ? "_" : arg.text).second)
          continue;
        command(none ? "Check the variant without " + keywords : "Check the variant with " + arg.text, json::array({uri, set, none ? "_" : arg.text}));
      }
    }
  }
  if (!variant.empty())
    command("Check the default variant again", json::array({uri}));
  // A [_Prop] that names no property: declare it.
  const json diagnostics = params.value("context", json::object()).value("diagnostics", json::array());
  for (const json &diagnostic : diagnostics) {
    std::string message = diagnostic.value("message", "");
    const std::string prefix = "Material property '";
    size_t close = message.find("' is not declared");
    if (analysis.kind != DocumentKind::ShaderLab || !message.starts_with(prefix) || close == std::string::npos)
      continue;
    std::string name = message.substr(prefix.size(), close - prefix.size());
    const Scope *properties = nullptr;
    for (const Scope &scope : analysis.shader.scopes) {
      if (scope.kind == ScopeKind::Properties && !properties)
        properties = &scope;
    }
    if (!properties || properties->span.end == 0 || analysis.text[properties->span.end - 1] != '}')
      continue;
    // Before the '}', on a line of its own, indented as the properties before it are or one level in from the '}'.
    size_t brace = properties->span.end - 1;
    size_t lineBegin = analysis.lines.lineStart(analysis.lines.lineOf(brace));
    std::string_view beforeBrace = std::string_view(analysis.text).substr(lineBegin, brace - lineBegin);
    std::string declaration = name + " (\"" + name + "\", Float) = 0";
    Span at{brace, brace};
    std::string text = declaration + " ";
    if (trim(beforeBrace).empty()) {
      std::string indent;
      if (!analysis.shader.properties.empty()) {
        size_t start = analysis.lines.lineStart(analysis.lines.lineOf(analysis.shader.properties.back().span.begin));
        for (size_t i = start; i < analysis.text.size() && (analysis.text[i] == ' ' || analysis.text[i] == '\t'); ++i)
          indent.push_back(analysis.text[i]);
      } else {
        indent = std::string(beforeBrace) + "    ";
      }
      std::string newline = analysis.text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
      at = {lineBegin, lineBegin};
      text = indent + declaration + newline;
    }
    json edit{{"range", toRange(analysis.text, analysis.lines, at, encoding)}, {"newText", text}};
    actions.push_back({{"title", "Declare " + name + " in Properties"}, {"kind", "quickfix"}, {"diagnostics", json::array({diagnostic})},
      {"edit", {{"changes", {{uri, json::array({edit})}}}}}});
  }
  return actions;
}
void Server::executeCommand(const json &params) {
  std::string command = params.value("command", "");
  json arguments = params.value("arguments", json::array());
  if (command != kCheckVariant || arguments.empty() || !arguments[0].is_string())
    throw RequestRefused("shaderlab-ls has no command " + command + " for these arguments.");
  std::string uri = arguments[0].get<std::string>();
  {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    if (it == documents_.end())
      throw RequestRefused("The document is not open.");
    std::vector<std::string> &variant = it->second.variant;
    if (arguments.size() < 3 || !arguments[1].is_array() || !arguments[2].is_string()) {
      variant.clear();
    } else {
      std::vector<std::string> set;
      for (const json &keyword : arguments[1]) {
        if (keyword.is_string())
          set.push_back(keyword.get<std::string>());
      }
      std::erase_if(variant, [&](const std::string &entry) {
        std::string keyword = entry[0] == '!' ? entry.substr(1) : entry;
        return std::find(set.begin(), set.end(), keyword) != set.end();
      });
      std::string chosen = arguments[2].get<std::string>();
      if (isNoKeyword(chosen)) {
        for (const std::string &keyword : set) {
          if (!isNoKeyword(keyword))
            variant.push_back("!" + keyword);
        }
      } else {
        variant.push_back(chosen);
      }
    }
  }
  reanalyze(uri);
}
json Server::formatEdits(const Analysis &analysis, std::optional<std::pair<size_t, size_t>> lines) {
  Encoding encoding;
  std::string clangFormat;
  {
    std::lock_guard lock(settingsMutex_);
    encoding = context_.encoding;
    clangFormat = clangFormatPath_;
  }
  // The layout comes from the .clang-format that applies to the file, or from Unity's defaults, never from the
  // editor's tabSize/insertSpaces: those would disagree with the code inside the HLSL blocks.
  FormatOptions options = resolveStyle(analysis.path, clangFormat);
  FormatResult result = analysis.kind == DocumentKind::ShaderLab ? formatShaderLab(analysis.text, options)
                                                                 : formatCode(analysis.text, options);
  if (!result.ok)
    throw RequestRefused("shaderlab-ls can't format this file: " + result.error);
  if (result.text == analysis.text)
    return json::array();
  if (!lines) {
    return json::array({{{"range", toRange(analysis.text, analysis.lines, {0, analysis.text.size()}, encoding)}, {"newText", std::move(result.text)}}});
  }
  // A range is formatted as part of the whole file, so that it comes out as it would then, and only the changes
  // that touch its lines are kept.
  std::vector<std::string_view> from = splitLines(analysis.text);
  std::vector<std::string_view> to = splitLines(result.text);
  std::vector<size_t> offsets{0};
  for (std::string_view line : from)
    offsets.push_back(offsets.back() + line.size());
  json edits = json::array();
  for (const LineHunk &hunk : diffLines(from, to)) {
    bool touches = hunk.fromBegin == hunk.fromEnd ? hunk.fromBegin >= lines->first && hunk.fromBegin <= lines->second
                                                  : hunk.fromBegin <= lines->second && hunk.fromEnd > lines->first;
    if (!touches)
      continue;
    std::string text;
    for (size_t j = hunk.toBegin; j < hunk.toEnd; ++j)
      text += to[j];
    Span span{offsets[hunk.fromBegin], offsets[hunk.fromEnd]};
    edits.push_back({{"range", toRange(analysis.text, analysis.lines, span, encoding)}, {"newText", std::move(text)}});
  }
  return edits;
}
json Server::initialize(const json &params) {
  Encoding encoding = Encoding::Utf16;
  const json capabilities = params.value("capabilities", json::object());
  if (capabilities.contains("general") && capabilities["general"].contains("positionEncodings")) {
    for (const json &value : capabilities["general"]["positionEncodings"]) {
      if (value == "utf-8")
        encoding = Encoding::Utf8;
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
  // The folders references and renames reach into: the workspace folders, or else the root of a client that
  // predates them.
  std::vector<std::filesystem::path> roots;
  if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array()) {
    for (const json &folder : params["workspaceFolders"]) {
      if (folder.is_object() && folder.contains("uri") && folder["uri"].is_string())
        roots.push_back(uriToPath(folder["uri"].get<std::string>()));
    }
  } else if (params.contains("rootUri") && params["rootUri"].is_string()) {
    roots.push_back(uriToPath(params["rootUri"].get<std::string>()));
  } else if (params.contains("rootPath") && params["rootPath"].is_string()) {
    std::string path = params["rootPath"].get<std::string>();
    roots.push_back(std::filesystem::path(std::u8string(path.begin(), path.end())));
  }
  {
    std::lock_guard lock(settingsMutex_);
    roots_ = std::move(roots);
  }
  if (params.contains("initializationOptions") && params["initializationOptions"].is_object()) {
    applySettings(params["initializationOptions"]);
  }
  try {
    watching_ = capabilities.at("workspace").at("didChangeWatchedFiles").value("dynamicRegistration", false);
  } catch (...) {
  }
  {
    std::lock_guard lock(settingsMutex_);
    index_.setCacheDirectory(indexCache_.value_or(Index::defaultCacheDirectory()));
    index_.setRoots(roots_, context_.editorOverride);
  }
  CheckOptions options;
  bool tokens;
  {
    std::lock_guard lock(settingsMutex_);
    options = checkOptions_;
    tokens = semanticTokens_.shaderLab || semanticTokens_.hlsl || semanticTokens_.glsl;
  }
  if (options.compiler != CompilerChoice::None)
    notify("window/logMessage", compilerLog(options));
  json result = {
    {"capabilities",
      {
        {"positionEncoding", encoding == Encoding::Utf8 ? "utf-8" : "utf-16"},
        {"textDocumentSync", {{"openClose", true}, {"change", 1}, {"save", {{"includeText", false}}}}},
        {"completionProvider", {{"triggerCharacters", {"[", "\"", "#", "."}}}},
        {"hoverProvider", true},
        {"signatureHelpProvider", {{"triggerCharacters", {"(", ","}}}},
        {"workspaceSymbolProvider", true},
        {"definitionProvider", true},
        {"documentSymbolProvider", true},
        {"referencesProvider", true},
        {"documentHighlightProvider", true},
        {"renameProvider", {{"prepareProvider", true}}},
        {"documentFormattingProvider", true},
        {"documentRangeFormattingProvider", true},
        {"codeActionProvider", {{"codeActionKinds", {"quickfix", "source"}}}},
        {"executeCommandProvider", {{"commands", {kCheckVariant}}}},
        {"workspace", {{"workspaceFolders", {{"supported", true}, {"changeNotifications", true}}}}},
      }},
    {"serverInfo", {{"name", "shaderlab-ls"}, {"version", SHADERLAB_LS_VERSION}}},
  };
  // For an editor with no grammar of its own for ShaderLab and HLSL. One that has a tree-sitter grammar for some of
  // them turns those off, rather than have the two paint over each other.
  if (tokens)
    result["capabilities"]["semanticTokensProvider"] = {{"legend", semanticTokensLegend()}, {"full", true}};
  return result;
}
// {
//   "unityEditorPath": "C:/Program Files/Unity/Hub/Editor/6000.6.0f1/Editor",
//   "clangFormatPath": "C:/Program Files/LLVM/bin/clang-format.exe",
//   "dxcPath": "/opt/dxc/lib/libdxcompiler.so",
//   "keywords": ["_NORMALMAP"],
//   "defines": ["MY_DEFINE", "OTHER=2"],
//   "diagnostics": { "compiler": "auto", "delay": 400 },
//   "semanticTokens": true or { "shaderlab": false, "hlsl": true, "glsl": false }
// }
void Server::applySettings(const json &settings) {
  std::lock_guard lock(settingsMutex_);
  if (settings.contains("unityEditorPath") && settings["unityEditorPath"].is_string()) {
    std::string path = settings["unityEditorPath"].get<std::string>();
    context_.editorOverride = std::filesystem::path(std::u8string(path.begin(), path.end()));
    checkOptions_.editorOverride = context_.editorOverride;
  }
  if (settings.contains("clangFormatPath") && settings["clangFormatPath"].is_string()) {
    clangFormatPath_ = settings["clangFormatPath"].get<std::string>();
  }
  // true or absent: the user's cache folder; false: none; a string: that folder. Read at initialize.
  if (settings.contains("indexCache")) {
    const json &cache = settings["indexCache"];
    if (cache.is_boolean())
      indexCache_ = cache.get<bool>() ? Index::defaultCacheDirectory() : std::filesystem::path();
    else if (cache.is_string()) {
      std::string folder = cache.get<std::string>();
      indexCache_ = std::filesystem::path(std::u8string(folder.begin(), folder.end()));
    }
  }
  // All languages or each on its own. Whether the server offers them at all is decided at initialize, by whether any
  // is on then.
  if (settings.contains("semanticTokens")) {
    const json &tokens = settings["semanticTokens"];
    if (tokens.is_boolean()) {
      semanticTokens_ = {tokens.get<bool>(), tokens.get<bool>(), tokens.get<bool>()};
    } else if (tokens.is_object()) {
      auto read = [&](const char *language, bool &on) {
        if (tokens.contains(language) && tokens[language].is_boolean())
          on = tokens[language].get<bool>();
      };
      read("shaderlab", semanticTokens_.shaderLab);
      read("hlsl", semanticTokens_.hlsl);
      read("glsl", semanticTokens_.glsl);
    }
  }
  if (settings.contains("dxcPath") && settings["dxcPath"].is_string()) {
    std::string path = settings["dxcPath"].get<std::string>();
    checkOptions_.dxcLibrary = std::filesystem::path(std::u8string(path.begin(), path.end()));
  }
  if (settings.contains("keywords") && settings["keywords"].is_array()) {
    checkOptions_.keywords.clear();
    for (const json &keyword : settings["keywords"]) {
      if (keyword.is_string())
        checkOptions_.keywords.push_back(keyword.get<std::string>());
    }
  }
  if (settings.contains("defines") && settings["defines"].is_array()) {
    checkOptions_.defines.clear();
    for (const json &define : settings["defines"]) {
      if (!define.is_string())
        continue;
      std::string text = define.get<std::string>();
      size_t equals = text.find('=');
      checkOptions_.defines.push_back(equals == std::string::npos ? ShaderDefine{text, "1"}
                                                                  : ShaderDefine{text.substr(0, equals), text.substr(equals + 1)});
    }
  }
  if (settings.contains("diagnostics") && settings["diagnostics"].is_object()) {
    const json &diagnostics = settings["diagnostics"];
    if (diagnostics.contains("compiler") && diagnostics["compiler"].is_string()) {
      std::string choice = diagnostics["compiler"].get<std::string>();
      checkOptions_.compiler = choice == "fxc"    ? CompilerChoice::Fxc
                               : choice == "dxc"  ? CompilerChoice::Dxc
                               : choice == "none" ? CompilerChoice::None
                                                  : CompilerChoice::Auto;
    }
    // The switch this setting replaced.
    if (diagnostics.contains("fxc") && diagnostics["fxc"] == false)
      checkOptions_.compiler = CompilerChoice::None;
    debounce_ = std::chrono::milliseconds(diagnostics.value("delay", static_cast<int>(debounce_.count())));
  }
}
AnalyzeOptions Server::analyzeOptions(const std::vector<std::string> &variant) {
  std::lock_guard lock(settingsMutex_);
  return {{variant, checkOptions_.keywords}, checkOptions_.defines, checkOptions_.editorOverride};
}
void Server::openOrChange(const std::string &uri, int version, std::string text) {
  std::vector<std::string> variant;
  {
    std::lock_guard lock(documentsMutex_);
    if (auto it = documents_.find(uri); it != documents_.end())
      variant = it->second.variant;
  }
  AnalyzeOptions options = analyzeOptions(variant);
  options.includers = includerAnalyses(uriToPath(uri));
  std::vector<std::shared_ptr<const Analysis>> includers = options.includers;
  auto analysis = analyze(uriToPath(uri), std::move(text), options);
  index_.documentChanged(analysis);
  // A document from outside the workspace folders brings its Unity project into the index.
  {
    std::filesystem::path editorOverride;
    bool outside;
    {
      std::lock_guard lock(settingsMutex_);
      editorOverride = context_.editorOverride;
      outside = !Workspace(roots_, {}).contains(analysis->path);
    }
    if (outside) {
      auto project = UnityProject::forFile(analysis->path, editorOverride);
      if (!project->root().empty())
        index_.addRoot(project->root());
    }
  }
  {
    std::lock_guard lock(documentsMutex_);
    Document &doc = documents_[uri];
    doc.version = version;
    doc.analysis = std::move(analysis);
    doc.includers = std::move(includers);
  }
  publish(uri);
  std::chrono::milliseconds delay;
  {
    std::lock_guard lock(settingsMutex_);
    delay = debounce_;
  }
  schedule(uri, delay);
}
void Server::publish(const std::string &uri) {
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
    if (it == documents_.end())
      return;
    const Analysis &analysis = *it->second.analysis;
    version = it->second.version;
    for (const Diagnostic &diagnostic : analysis.diagnostics)
      diagnostics.push_back(diagnosticJson(analysis, diagnostic, encoding));
    for (const Diagnostic &diagnostic : it->second.hlsl)
      diagnostics.push_back(diagnosticJson(analysis, diagnostic, encoding));
  }
  notify("textDocument/publishDiagnostics", {{"uri", uri}, {"version", version}, {"diagnostics", std::move(diagnostics)}});
}
void Server::schedule(const std::string &uri, std::chrono::milliseconds delay) {
  CheckOptions options;
  {
    std::lock_guard lock(settingsMutex_);
    options = checkOptions_;
  }
  if (!canCompileHlsl(uriToPath(uri), options))
    return;
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
    auto next = std::min_element(pending_.begin(), pending_.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    if (std::chrono::steady_clock::now() < next->second) {
      workerWake_.wait_until(lock, next->second);
      continue;
    }
    std::string uri = next->first;
    pending_.erase(next);
    lock.unlock();
    try {
      runCheck(uri);
    } catch (const std::exception &e) {
      std::fprintf(stderr, "shaderlab-ls: FXC check failed: %s\n", e.what());
    }
    lock.lock();
  }
}
void Server::runCheck(const std::string &uri) {
  std::shared_ptr<const Analysis> analysis;
  int version = 0;
  {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    if (it == documents_.end())
      return;
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
  {
    std::lock_guard lock(documentsMutex_);
    if (auto it = documents_.find(uri); it != documents_.end())
      options.variant = it->second.variant;
  }
  std::vector<Diagnostic> diagnostics = checkHlsl(*analysis, options, cancelled);
  // A variant other than the default is named on what it reports, so it is not mistaken for the default's.
  if (!options.variant.empty()) {
    std::string keywords;
    for (const std::string &keyword : options.variant)
      keywords += (keywords.empty() ? "" : ", ") + (keyword[0] == '!' ? "no " + keyword.substr(1) : keyword);
    for (Diagnostic &diagnostic : diagnostics)
      diagnostic.message += " [variant: " + keywords + "]";
  }
  {
    std::lock_guard lock(documentsMutex_);
    auto it = documents_.find(uri);
    if (it == documents_.end() || it->second.analysis != analysis || it->second.version != version)
      return;
    it->second.hlsl = std::move(diagnostics);
  }
  publish(uri);
}
} // namespace sls
