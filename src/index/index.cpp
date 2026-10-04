#include "index/index.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include "common/util.h"
#include "index/files.h"
#include "unity/project.h"
namespace fs = std::filesystem;
namespace sls {
namespace {
  enum FileFlag : uint8_t {
    kIndexed = 1, // its facts are in the tables
    kWorkspace = 2, // in a workspace folder, rather than a package or the editor reached by an #include
    kOpen = 4, // stood for by an open document
    kDeleted = 8,
  };
  constexpr auto kRefreshInterval = std::chrono::seconds(5);
  constexpr auto kSaveInterval = std::chrono::seconds(10);
  // Every identifier in `text` outside comments and strings, ShaderLab and directives included: `[_Prop]` and
  // `#pragma vertex vert` name things as much as code does.
  std::vector<std::string_view> identifiers(std::string_view text) {
    std::vector<std::string_view> result;
    size_t i = 0;
    while (i < text.size()) {
      char c = text[i];
      if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
        while (i < text.size() && text[i] != '\n')
          ++i;
      } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
        size_t close = text.find("*/", i + 2);
        i = close == std::string_view::npos ? text.size() : close + 2;
      } else if (c == '"') {
        ++i;
        while (i < text.size() && text[i] != '"' && text[i] != '\n')
          i += text[i] == '\\' ? 2 : 1;
        ++i;
      } else if (isIdentStart(c)) {
        size_t begin = i;
        while (i < text.size() && isIdentChar(text[i]))
          ++i;
        result.push_back(text.substr(begin, i - begin));
      } else if (std::isdigit(static_cast<unsigned char>(c))) {
        while (i < text.size() && isIdentChar(text[i]))
          ++i;
      } else {
        ++i;
      }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }
  bool fuzzyMatch(std::string_view name, std::string_view query) {
    size_t at = 0;
    for (char c : name) {
      if (at < query.size() && std::tolower(static_cast<unsigned char>(c)) == std::tolower(static_cast<unsigned char>(query[at])))
        ++at;
    }
    return at == query.size();
  }
  // Drops `value` from an unordered array of ids.
  void erase(std::vector<uint32_t> &ids, uint32_t value) {
    auto it = std::find(ids.begin(), ids.end(), value);
    if (it != ids.end()) {
      *it = ids.back();
      ids.pop_back();
    }
  }
} // namespace
Index::Index() {
  thread_ = std::thread([this] { run(); });
}
Index::~Index() {
  {
    std::lock_guard lock(queueMutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable())
    thread_.join();
  save();
}
fs::path Index::defaultCacheDirectory() {
#ifdef _WIN32
  std::string base = environmentVariable("LOCALAPPDATA");
  return base.empty() ? fs::path() : fs::path(std::u8string(base.begin(), base.end())) / "shaderlab-ls" / "index";
#else
  std::string base = environmentVariable("XDG_CACHE_HOME");
  if (!base.empty())
    return fs::path(base) / "shaderlab-ls" / "index";
  std::string home = environmentVariable("HOME");
  return home.empty() ? fs::path() : fs::path(home) / ".cache" / "shaderlab-ls" / "index";
#endif
}
void Index::setCacheDirectory(fs::path directory) {
  std::lock_guard lock(queueMutex_);
  cacheDirectory_ = std::move(directory);
}
void Index::onLog(std::function<void(const std::string &)> log) {
  std::lock_guard lock(queueMutex_);
  log_ = std::move(log);
}
void Index::onIdle(std::function<void()> callback) {
  std::lock_guard lock(queueMutex_);
  idleCallback_ = std::move(callback);
}
void Index::setRoots(std::vector<fs::path> roots, fs::path editorOverride) {
  {
    std::lock_guard lock(queueMutex_);
    roots_ = std::move(roots);
    editorOverride_ = std::move(editorOverride);
    crawlPending_ = true;
    // One saved index per set of folders and editor: FNV-1a of their keys names its file.
    if (!cacheDirectory_.empty() && !roots_.empty()) {
      std::vector<std::string> keys;
      for (const fs::path &root : roots_)
        keys.push_back(pathKey(root));
      std::sort(keys.begin(), keys.end());
      keys.push_back(pathKey(editorOverride_));
      uint64_t hash = 1469598103934665603ull;
      for (const std::string &key : keys) {
        for (char c : key + "|")
          hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
      }
      char name[32];
      std::snprintf(name, sizeof name, "%016llx.idx", static_cast<unsigned long long>(hash));
      cacheFile_ = cacheDirectory_ / name;
      loadPending_ = true;
    } else {
      cacheFile_.clear();
    }
  }
  wake_.notify_all();
}
void Index::addRoot(const fs::path &root) {
  {
    std::lock_guard lock(queueMutex_);
    if (underRoots(root / "x", roots_))
      return;
    roots_.push_back(root);
    crawlPending_ = true;
  }
  wake_.notify_all();
}
void Index::fileChanged(const fs::path &path) {
  if (!isShaderSource(path))
    return;
  {
    std::shared_lock lock(tables_);
    auto it = fileIds_.find(pathKey(path));
    if (it != fileIds_.end() && (fileFlags_[it->second] & kOpen))
      return; // the open document stands for it
  }
  enqueue({path, nullptr, false});
}
void Index::fileDeleted(const fs::path &path) {
  enqueue({path, nullptr, true});
}
void Index::documentChanged(std::shared_ptr<const Analysis> analysis) {
  if (analysis && isShaderSource(analysis->path))
    enqueue({analysis->path, std::move(analysis), false});
}
void Index::documentClosed(const fs::path &path) {
  if (isShaderSource(path))
    enqueue({path, nullptr, false});
}
void Index::refresh() {
  {
    std::lock_guard lock(queueMutex_);
    if (crawlPending_ || std::chrono::steady_clock::now() - lastCrawl_ < kRefreshInterval)
      return;
    crawlPending_ = true;
  }
  wake_.notify_all();
}
bool Index::waitReady(std::chrono::milliseconds timeout) const {
  std::unique_lock lock(queueMutex_);
  // Until what is queued is in, so that a change just reported is part of the answer; past the timeout, the answer
  // is what there is, once the first crawl is in.
  idle_.wait_for(lock, timeout, [&] { return ready_ && queue_.empty() && !busy_ && !crawlPending_; });
  return ready_;
}
uint64_t Index::generation() const {
  std::shared_lock lock(tables_);
  return generation_;
}
void Index::enqueue(Job job) {
  {
    std::lock_guard lock(queueMutex_);
    std::string key = pathKey(job.path);
    // One job per file: the latest replaces one still waiting.
    auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Job &queued) { return pathKey(queued.path) == key; });
    if (it != queue_.end())
      *it = std::move(job);
    else
      queue_.push_back(std::move(job));
  }
  wake_.notify_all();
}
void Index::run() {
  while (true) {
    Job job;
    bool crawling = false;
    {
      std::unique_lock lock(queueMutex_);
      wake_.wait(lock, [&] { return stopping_ || crawlPending_ || loadPending_ || !queue_.empty(); });
      if (stopping_)
        return;
      busy_ = true;
      if (loadPending_) {
        loadPending_ = false;
        lock.unlock();
        load(); // before the crawl, which then only looks for what changed
        lock.lock();
      }
      if (crawlPending_) {
        crawlPending_ = false;
        crawling = true;
      } else {
        job = std::move(queue_.front());
        queue_.pop_front();
      }
    }
    if (crawling) {
      crawl();
    } else if (job.deleted) {
      remove(pathKey(job.path));
    } else if (auto facts = read(job)) {
      std::vector<fs::path> includes = facts->includes;
      bool workspace = inWorkspace(job.path);
      store(std::move(*facts), workspace, job.analysis != nullptr);
      // The files it includes are indexed too, packages and the editor's among them, so that what includes what is
      // known all the way down.
      for (const fs::path &include : includes) {
        bool known;
        {
          std::shared_lock lock(tables_);
          auto it = fileIds_.find(pathKey(include));
          known = it != fileIds_.end() && (fileFlags_[it->second] & kIndexed);
        }
        if (!known)
          enqueue({include, nullptr, false});
      }
    }
    std::function<void()> callback;
    bool idle;
    {
      std::lock_guard lock(queueMutex_);
      busy_ = false;
      idle = queue_.empty() && !crawlPending_;
      if (idle) {
        ready_ = true;
        idle_.notify_all();
        callback = idleCallback_;
      }
    }
    if (idle) {
      if (std::chrono::steady_clock::now() - lastSave_ > kSaveInterval)
        save();
      if (callback)
        callback();
    }
  }
}
bool Index::inWorkspace(const fs::path &path) const {
  std::lock_guard lock(queueMutex_);
  return underRoots(path, roots_);
}
void Index::crawl() {
  std::vector<fs::path> roots;
  {
    std::lock_guard lock(queueMutex_);
    roots = roots_;
    lastCrawl_ = std::chrono::steady_clock::now();
  }
  std::unordered_set<std::string> seen;
  constexpr size_t kMaxEntries = 200000;
  size_t visited = 0;
  for (const fs::path &root : roots) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator() && ++visited < kMaxEntries; it.increment(ec)) {
      {
        std::lock_guard lock(queueMutex_);
        if (stopping_)
          return;
      }
      std::error_code typeError;
      if (it->is_directory(typeError)) {
        if (isSkippedFolder(it->path().filename()))
          it.disable_recursion_pending();
        continue;
      }
      if (!isShaderSource(it->path()))
        continue;
      std::string key = pathKey(it->path());
      seen.insert(key);
      std::error_code timeError;
      auto time = fs::last_write_time(it->path(), timeError);
      bool current;
      {
        std::shared_lock lock(tables_);
        auto found = fileIds_.find(key);
        current = found != fileIds_.end() && (fileFlags_[found->second] & kIndexed) && !(fileFlags_[found->second] & kDeleted) &&
                  ((fileFlags_[found->second] & kOpen) || fileTime_[found->second] == time);
      }
      if (!current)
        enqueue({it->path(), nullptr, false});
    }
  }
  // Workspace files the crawl no longer finds were deleted, or are no longer in a workspace folder.
  std::vector<fs::path> gone;
  {
    std::shared_lock lock(tables_);
    for (const auto &[key, id] : fileIds_) {
      uint8_t flags = fileFlags_[id];
      if ((flags & kWorkspace) && !(flags & (kOpen | kDeleted)) && !seen.contains(key))
        gone.push_back(filePath_[id]);
    }
  }
  // The package and editor files reached through #include are not in the folders crawled; they are checked on their
  // own, so that an updated package is indexed again.
  std::vector<std::pair<fs::path, fs::file_time_type>> outside;
  {
    std::shared_lock lock(tables_);
    for (FileId file = 0; file < filePath_.size(); ++file) {
      if ((fileFlags_[file] & (kIndexed | kWorkspace | kDeleted | kOpen)) == kIndexed)
        outside.emplace_back(filePath_[file], fileTime_[file]);
    }
  }
  for (const auto &[path, time] : outside) {
    std::error_code ec;
    auto now = fs::last_write_time(path, ec);
    if (ec)
      enqueue({path, nullptr, true});
    else if (now != time)
      enqueue({path, nullptr, false});
  }
  for (const fs::path &path : gone)
    enqueue({path, nullptr, true});
}
std::optional<Index::Facts> Index::read(const Job &job) const {
  fs::path editorOverride;
  {
    std::lock_guard lock(queueMutex_);
    editorOverride = editorOverride_;
  }
  std::shared_ptr<const Analysis> analysis = job.analysis;
  Facts facts;
  facts.key = pathKey(job.path);
  facts.path = job.path;
  if (!analysis) {
    auto text = readCached(job.path);
    if (!text)
      return std::nullopt;
    std::error_code ec;
    facts.time = fs::last_write_time(job.path, ec);
    AnalyzeOptions options;
    options.editorOverride = editorOverride;
    analysis = analyze(job.path, *text, options);
  }
  const Analysis &a = *analysis;
  for (std::string_view name : identifiers(a.text))
    facts.names.emplace_back(name);
  auto add = [&](const std::string &name, const std::string &container, uint8_t kind, Span span) {
    Position begin8 = a.lines.toPosition(a.text, span.begin, Encoding::Utf8);
    Position end8 = a.lines.toPosition(a.text, span.end, Encoding::Utf8);
    Position begin16 = a.lines.toPosition(a.text, span.begin, Encoding::Utf16);
    Position end16 = a.lines.toPosition(a.text, span.end, Encoding::Utf16);
    facts.decls.push_back({name, container, kind, static_cast<uint32_t>(begin8.line), static_cast<uint32_t>(begin8.character),
      static_cast<uint32_t>(begin16.character), static_cast<uint32_t>(std::max(end8.character - begin8.character, 0)),
      static_cast<uint32_t>(std::max(end16.character - begin16.character, 0))});
  };
  if (a.kind == DocumentKind::ShaderLab && a.shader.hasShader && !a.shader.name.empty())
    add(a.shader.name, {}, kShaderName, a.shader.nameSpan.empty() ? a.shader.keyword : a.shader.nameSpan);
  auto project = UnityProject::forFile(job.path, editorOverride);
  for (size_t u = 0; u < a.units.size(); ++u) {
    if (a.isGlsl(u))
      continue;
    for (const HlslDecl &decl : a.units[u].scan.decls)
      add(decl.name, decl.container, static_cast<uint8_t>(decl.kind), decl.nameSpan);
    for (const HlslInclude &include : a.units[u].scan.includes) {
      if (auto resolved = project->resolveInclude(include.path, job.path.parent_path()))
        facts.includes.push_back(*resolved);
    }
  }
  return facts;
}
Index::NameId Index::intern(std::string_view name) {
  if (auto it = nameIds_.find(name); it != nameIds_.end())
    return it->second;
  std::string_view stored = nameStore_.emplace_back(name);
  NameId id = static_cast<NameId>(nameText_.size());
  nameText_.push_back(stored);
  nameIds_.emplace(stored, id);
  namePostings_.emplace_back();
  return id;
}
Index::FileId Index::fileFor(const std::string &key, const fs::path &path) {
  if (auto it = fileIds_.find(key); it != fileIds_.end())
    return it->second;
  FileId id = static_cast<FileId>(filePath_.size());
  fileIds_.emplace(key, id);
  filePath_.push_back(path);
  fileTime_.emplace_back();
  fileFlags_.push_back(0);
  fileNames_.emplace_back();
  fileDecls_.emplace_back();
  fileIncludes_.emplace_back();
  fileIncluders_.emplace_back();
  return id;
}
void Index::store(Facts facts, bool workspace, bool open) {
  std::unique_lock lock(tables_);
  FileId file = fileFor(facts.key, facts.path);
  filePath_[file] = facts.path;
  // Names: patch the postings of the names it no longer has and the ones it now has, nothing else.
  std::vector<NameId> names;
  names.reserve(facts.names.size());
  for (const std::string &name : facts.names)
    names.push_back(intern(name));
  std::sort(names.begin(), names.end());
  const std::vector<NameId> &old = fileNames_[file];
  std::vector<NameId> gone, added;
  std::set_difference(old.begin(), old.end(), names.begin(), names.end(), std::back_inserter(gone));
  std::set_difference(names.begin(), names.end(), old.begin(), old.end(), std::back_inserter(added));
  for (NameId name : gone)
    erase(namePostings_[name], file);
  for (NameId name : added)
    namePostings_[name].push_back(file);
  fileNames_[file] = std::move(names);
  std::vector<DeclRow> rows;
  rows.reserve(facts.decls.size());
  for (const Facts::Decl &decl : facts.decls)
    rows.push_back({intern(decl.name), decl.container.empty() ? kNoName : intern(decl.container), decl.kind, decl.line, decl.column8, decl.column16, decl.length8, decl.length16});
  fileDecls_[file] = std::move(rows);
  // Includes, and the includers of what it includes.
  for (FileId included : fileIncludes_[file])
    erase(fileIncluders_[included], file);
  std::vector<FileId> includes;
  for (const fs::path &include : facts.includes) {
    FileId included = fileFor(pathKey(include), include);
    if (included != file && std::find(includes.begin(), includes.end(), included) == includes.end()) {
      includes.push_back(included);
      fileIncluders_[included].push_back(file);
    }
  }
  fileIncludes_[file] = std::move(includes);
  fileTime_[file] = facts.time;
  fileFlags_[file] = static_cast<uint8_t>(kIndexed | (workspace ? kWorkspace : 0) | (open ? kOpen : 0));
  ++generation_;
}
void Index::remove(const std::string &key) {
  std::unique_lock lock(tables_);
  auto it = fileIds_.find(key);
  if (it == fileIds_.end())
    return;
  FileId file = it->second;
  for (NameId name : fileNames_[file])
    erase(namePostings_[name], file);
  for (FileId included : fileIncludes_[file])
    erase(fileIncluders_[included], file);
  fileNames_[file].clear();
  fileDecls_[file].clear();
  fileIncludes_[file].clear();
  // Files that include it keep their edges, so that it is in the graph again if it comes back.
  fileFlags_[file] = kDeleted;
  ++generation_;
}
std::vector<fs::path> Index::workspaceFiles() const {
  std::shared_lock lock(tables_);
  std::vector<fs::path> result;
  for (FileId file = 0; file < filePath_.size(); ++file) {
    if ((fileFlags_[file] & (kIndexed | kWorkspace | kDeleted)) == (kIndexed | kWorkspace))
      result.push_back(filePath_[file]);
  }
  return result;
}
std::vector<fs::path> Index::filesNaming(std::string_view name) const {
  std::shared_lock lock(tables_);
  std::vector<fs::path> result;
  auto it = nameIds_.find(name);
  if (it == nameIds_.end())
    return result;
  for (FileId file : namePostings_[it->second]) {
    if ((fileFlags_[file] & (kWorkspace | kDeleted)) == kWorkspace)
      result.push_back(filePath_[file]);
  }
  return result;
}
std::vector<fs::path> Index::includers(const fs::path &path, size_t limit) const {
  std::shared_lock lock(tables_);
  std::vector<fs::path> result;
  auto it = fileIds_.find(pathKey(path));
  if (it == fileIds_.end())
    return result;
  // Breadth first, so the nearest come first.
  std::vector<FileId> order{it->second};
  std::unordered_set<FileId> seen{it->second};
  for (size_t at = 0; at < order.size() && result.size() < limit; ++at) {
    for (FileId includer : fileIncluders_[order[at]]) {
      if (!seen.insert(includer).second || (fileFlags_[includer] & kDeleted))
        continue;
      order.push_back(includer);
      result.push_back(filePath_[includer]);
    }
  }
  return result;
}
std::vector<fs::path> Index::directIncluders(const fs::path &path) const {
  std::shared_lock lock(tables_);
  std::vector<fs::path> result;
  if (auto it = fileIds_.find(pathKey(path)); it != fileIds_.end()) {
    for (FileId includer : fileIncluders_[it->second]) {
      if (!(fileFlags_[includer] & kDeleted))
        result.push_back(filePath_[includer]);
    }
  }
  return result;
}
namespace {
  // The saved index's format: the tables as they are, ids and all. A file of another format, or written by another
  // version of the server, which may index differently, is not read.
  constexpr char kMagic[8] = {'S', 'L', 'S', 'I', 'N', 'D', 'E', 'X'};
  constexpr uint32_t kFormat = 1;
  class Writer {
  public:
    explicit Writer(std::ofstream &out)
      : out_(out) {}
    void u8(uint8_t value) {
      out_.put(static_cast<char>(value));
    }
    void u32(uint32_t value) {
      out_.write(reinterpret_cast<const char *>(&value), sizeof value);
    }
    void i64(int64_t value) {
      out_.write(reinterpret_cast<const char *>(&value), sizeof value);
    }
    void text(std::string_view value) {
      u32(static_cast<uint32_t>(value.size()));
      out_.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    void ids(const std::vector<uint32_t> &values) {
      u32(static_cast<uint32_t>(values.size()));
      out_.write(reinterpret_cast<const char *>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(uint32_t)));
    }
  private:
    std::ofstream &out_;
  };
  // Reads what Writer wrote. A short read or an id out of range fails it, and everything read is then dropped.
  class Reader {
  public:
    explicit Reader(std::string_view data)
      : data_(data) {}
    bool failed() const {
      return failed_;
    }
    void fail() {
      failed_ = true;
    }
    uint8_t u8() {
      return take(1) ? static_cast<uint8_t>(data_[at_ - 1]) : 0;
    }
    uint32_t u32() {
      uint32_t value = 0;
      if (take(sizeof value))
        std::memcpy(&value, data_.data() + at_ - sizeof value, sizeof value);
      return value;
    }
    int64_t i64() {
      int64_t value = 0;
      if (take(sizeof value))
        std::memcpy(&value, data_.data() + at_ - sizeof value, sizeof value);
      return value;
    }
    std::string_view text() {
      uint32_t size = u32();
      return take(size) ? data_.substr(at_ - size, size) : std::string_view();
    }
    std::vector<uint32_t> ids(uint32_t limit) {
      uint32_t count = u32();
      std::vector<uint32_t> values;
      if (!take(static_cast<size_t>(count) * sizeof(uint32_t)))
        return values;
      values.resize(count);
      std::memcpy(values.data(), data_.data() + at_ - values.size() * sizeof(uint32_t), values.size() * sizeof(uint32_t));
      for (uint32_t value : values)
        failed_ = failed_ || value >= limit;
      return values;
    }
  private:
    bool take(size_t size) {
      if (failed_ || data_.size() - at_ < size) {
        failed_ = true;
        return false;
      }
      at_ += size;
      return true;
    }
    std::string_view data_;
    size_t at_ = 0;
    bool failed_ = false;
  };
} // namespace
void Index::save() {
  fs::path file;
  std::function<void(const std::string &)> log;
  {
    std::lock_guard lock(queueMutex_);
    file = cacheFile_;
    log = log_;
  }
  std::shared_lock lock(tables_);
  if (file.empty() || generation_ == savedGeneration_)
    return;
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  // Written beside it and moved over it, so that a crash mid-write leaves the last good one.
  fs::path temporary = file;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    Writer write(out);
    out.write(kMagic, sizeof kMagic);
    write.u32(kFormat);
    write.text(SHADERLAB_LS_VERSION);
    write.u32(static_cast<uint32_t>(nameText_.size()));
    for (std::string_view name : nameText_)
      write.text(name);
    write.u32(static_cast<uint32_t>(filePath_.size()));
    static const std::vector<uint32_t> kNothing;
    static const std::vector<DeclRow> kNoRows;
    for (FileId f = 0; f < filePath_.size(); ++f) {
      std::u8string path = filePath_[f].u8string();
      write.text(std::string_view(reinterpret_cast<const char *>(path.data()), path.size()));
      write.i64(static_cast<int64_t>(fileTime_[f].time_since_epoch().count()));
      // An open document's facts are of text that may never be saved: it is read from disk next time.
      bool open = fileFlags_[f] & kOpen;
      write.u8(static_cast<uint8_t>(fileFlags_[f] & (open ? kWorkspace : (kIndexed | kWorkspace | kDeleted))));
      write.ids(open ? kNothing : fileNames_[f]);
      const std::vector<DeclRow> &rows = open ? kNoRows : fileDecls_[f];
      write.u32(static_cast<uint32_t>(rows.size()));
      for (const DeclRow &row : rows) {
        for (uint32_t value : {row.name, row.container, static_cast<uint32_t>(row.kind), row.line, row.column8, row.column16, row.length8, row.length16})
          write.u32(value);
      }
      write.ids(open ? kNothing : fileIncludes_[f]);
    }
    if (!out) {
      if (log)
        log("The index could not be saved to " + displayPath(file) + ".");
      return;
    }
  }
  fs::rename(temporary, file, ec);
  if (ec) {
    fs::remove(temporary, ec);
    if (log)
      log("The index could not be saved to " + displayPath(file) + ": " + ec.message());
    return;
  }
  savedGeneration_ = generation_;
  lastSave_ = std::chrono::steady_clock::now();
  // One file per set of folders adds up: those of projects no one has opened for a month go.
  auto now = fs::file_time_type::clock::now();
  for (fs::directory_iterator it(file.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code timeError;
    auto written = it->last_write_time(timeError);
    if (!timeError && it->path().extension() == ".idx" && now - written > std::chrono::hours(24 * 30)) {
      std::error_code removeError;
      fs::remove(it->path(), removeError);
    }
  }
}
void Index::load() {
  fs::path file;
  std::function<void(const std::string &)> log;
  {
    std::lock_guard lock(queueMutex_);
    file = cacheFile_;
    log = log_;
  }
  auto bytes = file.empty() ? std::nullopt : readFile(file);
  if (!bytes)
    return;
  std::unique_lock lock(tables_);
  if (!filePath_.empty())
    return; // indexed already this session, which is newer
  if (bytes->size() < sizeof kMagic || std::memcmp(bytes->data(), kMagic, sizeof kMagic) != 0)
    return;
  Reader read(std::string_view(*bytes).substr(sizeof kMagic));
  if (read.u32() != kFormat || read.text() != SHADERLAB_LS_VERSION)
    return;
  uint32_t names = read.u32();
  for (uint32_t n = 0; n < names && !read.failed(); ++n)
    intern(read.text());
  if (nameText_.size() != names)
    read.fail(); // a name twice: not a file this server wrote
  uint32_t files = read.u32();
  for (uint32_t f = 0; f < files && !read.failed(); ++f) {
    std::string_view path = read.text();
    fs::path filePath(std::u8string(path.begin(), path.end()));
    if (fileFor(pathKey(filePath), filePath) != f) {
      read.fail();
      break;
    }
    fileTime_[f] = fs::file_time_type(fs::file_time_type::duration(read.i64()));
    fileFlags_[f] = read.u8();
    fileNames_[f] = read.ids(names);
    uint32_t rows = read.u32();
    for (uint32_t r = 0; r < rows && !read.failed(); ++r) {
      DeclRow row{};
      row.name = read.u32();
      row.container = read.u32();
      row.kind = static_cast<uint8_t>(read.u32());
      row.line = read.u32();
      row.column8 = read.u32();
      row.column16 = read.u32();
      row.length8 = read.u32();
      row.length16 = read.u32();
      if (row.name >= names || (row.container != kNoName && row.container >= names))
        read.fail();
      fileDecls_[f].push_back(row);
    }
    fileIncludes_[f] = read.ids(files);
  }
  if (read.failed() || filePath_.size() != files) {
    // Everything read is dropped, and the crawl indexes everything as it would have.
    nameStore_.clear();
    nameText_.clear();
    nameIds_.clear();
    namePostings_.clear();
    fileIds_.clear();
    filePath_.clear();
    fileTime_.clear();
    fileFlags_.clear();
    fileNames_.clear();
    fileDecls_.clear();
    fileIncludes_.clear();
    fileIncluders_.clear();
    if (log)
      log("The saved index at " + displayPath(file) + " could not be read; indexing from scratch.");
    return;
  }
  // The relations kept both ways are saved one way.
  for (FileId f = 0; f < filePath_.size(); ++f) {
    for (NameId name : fileNames_[f])
      namePostings_[name].push_back(f);
    for (FileId included : fileIncludes_[f])
      fileIncluders_[included].push_back(f);
  }
  ++generation_;
  savedGeneration_ = generation_;
  if (log)
    log("Index: " + std::to_string(files) + " files loaded from " + displayPath(file) + ".");
}
std::vector<fs::path> Index::included(const std::vector<fs::path> &from, size_t limit) const {
  std::shared_lock lock(tables_);
  std::vector<FileId> order;
  std::unordered_set<FileId> seen;
  for (const fs::path &path : from) {
    if (auto it = fileIds_.find(pathKey(path)); it != fileIds_.end() && seen.insert(it->second).second)
      order.push_back(it->second);
  }
  std::vector<fs::path> result;
  for (size_t at = 0; at < order.size() && result.size() < limit; ++at) {
    for (FileId include : fileIncludes_[order[at]]) {
      if (!seen.insert(include).second)
        continue;
      order.push_back(include);
      result.push_back(filePath_[include]);
    }
  }
  return result;
}
std::vector<Index::Symbol> Index::symbols(std::string_view query, Encoding encoding, size_t limit) const {
  std::shared_lock lock(tables_);
  // Each distinct name is matched once, however many files declare it; the rows are then filtered by the result.
  std::vector<bool> matches(nameText_.size());
  for (NameId name = 0; name < nameText_.size(); ++name)
    matches[name] = fuzzyMatch(nameText_[name], query);
  std::vector<Symbol> result;
  for (FileId file = 0; file < filePath_.size() && result.size() < limit; ++file) {
    if ((fileFlags_[file] & (kIndexed | kWorkspace | kDeleted)) != (kIndexed | kWorkspace))
      continue;
    for (const DeclRow &row : fileDecls_[file]) {
      if (!matches[row.name] || row.kind == static_cast<uint8_t>(DeclKind::Field))
        continue;
      bool utf8 = encoding == Encoding::Utf8;
      result.push_back({std::string(nameText_[row.name]), row.container == kNoName ? std::string() : std::string(nameText_[row.container]), row.kind,
        filePath_[file], row.line, utf8 ? row.column8 : row.column16, utf8 ? row.length8 : row.length16});
      if (result.size() >= limit)
        break;
    }
  }
  return result;
}
} // namespace sls
