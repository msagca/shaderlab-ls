#include "index/files.h"
#include <map>
#include <mutex>
#include "common/util.h"
namespace fs = std::filesystem;
namespace sls {
bool isShaderSource(const fs::path &path) {
  std::string extension = toLower(path.extension().string());
  return extension == ".shader" || extension == ".hlsl" || extension == ".cginc" || extension == ".hlslinc" || extension == ".compute";
}
bool isSkippedFolder(const fs::path &name) {
  std::string text = name.string();
  if (text.empty())
    return false;
  if (text.front() == '.' || text.back() == '~')
    return true;
  for (const char *generated : {"Library", "Temp", "Logs", "obj", "UserSettings", "Build", "Builds", "node_modules"}) {
    if (iequals(text, generated))
      return true;
  }
  return false;
}
std::shared_ptr<const std::string> readCached(const fs::path &path) {
  static std::mutex mutex;
  static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const std::string>>> cache;
  std::error_code ec;
  auto time = fs::last_write_time(path, ec);
  if (ec)
    return nullptr;
  std::string key = pathKey(path);
  {
    std::lock_guard lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.first == time)
      return it->second.second;
  }
  auto text = readFile(path);
  if (!text)
    return nullptr;
  auto shared = std::make_shared<const std::string>(std::move(*text));
  std::lock_guard lock(mutex);
  cache[key] = {time, shared};
  return shared;
}
bool underRoots(const fs::path &path, const std::vector<fs::path> &roots) {
  std::string key = pathKey(path);
  for (const fs::path &root : roots) {
    std::string rootKey = pathKey(root);
    if (!rootKey.empty() && rootKey.back() != '/')
      rootKey += '/';
    if (!key.starts_with(rootKey))
      continue;
    // The folders between the root and the file. Keys are lowercase where case is ignored, which Unity's folder
    // names compare equal in as well.
    std::string_view relative = std::string_view(key).substr(rootKey.size());
    bool skipped = false;
    for (size_t slash = relative.find('/'); slash != std::string_view::npos; slash = relative.find('/')) {
      skipped = skipped || isSkippedFolder(fs::path(std::string(relative.substr(0, slash))));
      relative.remove_prefix(slash + 1);
    }
    if (!skipped)
      return true;
  }
  return false;
}
} // namespace sls
