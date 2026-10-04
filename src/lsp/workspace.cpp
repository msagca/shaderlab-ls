#include "lsp/workspace.h"
#include <mutex>
#include <set>
#include "common/util.h"
#include "index/files.h"
namespace fs = std::filesystem;
namespace sls {
namespace {
  constexpr size_t kMaxFiles = 20000;
} // namespace
Workspace::Workspace(std::vector<fs::path> roots, std::map<std::string, OpenDocument> open, const Index *index)
  : roots_(std::move(roots)), open_(std::move(open)), index_(index) {}
bool Workspace::contains(const fs::path &path) const {
  return underRoots(path, roots_);
}
std::vector<fs::path> Workspace::files() const {
  return filesMentioning({});
}
std::vector<fs::path> Workspace::filesMentioning(std::string_view word) const {
  std::vector<fs::path> result;
  std::set<std::string> seen;
  // Open documents as they are in the editor, which the index may not have caught up with.
  for (const auto &[key, document] : open_) {
    if (isShaderSource(document.analysis->path) && document.analysis->text.find(word) != std::string::npos && seen.insert(key).second)
      result.push_back(document.analysis->path);
  }
  if (index_) {
    for (fs::path &path : word.empty() ? index_->workspaceFiles() : index_->filesNaming(word)) {
      if (seen.insert(pathKey(path)).second)
        result.push_back(std::move(path));
    }
    return result;
  }
  // No index yet: read the folders.
  size_t visited = 0;
  for (const fs::path &root : roots_) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (++visited > kMaxFiles)
        return result;
      const fs::directory_entry &entry = *it;
      std::error_code typeError;
      if (entry.is_directory(typeError)) {
        if (isSkippedFolder(entry.path().filename()))
          it.disable_recursion_pending();
        continue;
      }
      if (!isShaderSource(entry.path()) || open_.contains(pathKey(entry.path())))
        continue;
      auto text = word.empty() ? nullptr : readCached(entry.path());
      if ((word.empty() || (text && text->find(word) != std::string::npos)) && seen.insert(pathKey(entry.path())).second)
        result.push_back(entry.path());
    }
  }
  return result;
}
std::optional<std::vector<fs::path>> Workspace::includers(const fs::path &path) const {
  if (!index_)
    return std::nullopt;
  return index_->includers(path);
}
std::optional<std::vector<fs::path>> Workspace::included(const std::vector<fs::path> &from) const {
  if (!index_)
    return std::nullopt;
  return index_->included(from);
}
std::shared_ptr<const Analysis> Workspace::analysis(const fs::path &path) const {
  std::string key = pathKey(path);
  if (auto it = open_.find(key); it != open_.end())
    return it->second.analysis;
  static std::mutex mutex;
  static std::map<std::string, std::pair<std::shared_ptr<const std::string>, std::shared_ptr<const Analysis>>> cache;
  auto text = readCached(path);
  if (!text)
    return nullptr;
  {
    std::lock_guard lock(mutex);
    // The same text as last time is the same file as last time: readCached hands out one string per version.
    if (auto it = cache.find(key); it != cache.end() && it->second.first == text)
      return it->second.second;
  }
  auto analysis = analyze(path, *text);
  std::lock_guard lock(mutex);
  cache[key] = {text, analysis};
  return analysis;
}
std::string Workspace::uri(const fs::path &path) const {
  if (auto it = open_.find(pathKey(path)); it != open_.end())
    return it->second.uri;
  return pathToUri(path);
}
} // namespace sls
