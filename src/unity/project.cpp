#include "unity/project.h"
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/util.h"
#include "compiler/dxc.h"
namespace fs = std::filesystem;
namespace sls {
namespace {
  bool isDirectory(const fs::path &path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
  }
  bool isFile(const fs::path &path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
  }
  fs::path findProjectRoot(const fs::path &file) {
    for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
      if (isDirectory(dir / "Assets") && isDirectory(dir / "ProjectSettings"))
        return dir;
      if (dir == dir.root_path() || dir.parent_path() == dir)
        break;
    }
    return {};
  }
  std::string readEditorVersion(const fs::path &root) {
    auto text = readFile(root / "ProjectSettings" / "ProjectVersion.txt");
    if (!text)
      return {};
    std::istringstream lines(*text);
    std::string line;
    while (std::getline(lines, line)) {
      std::string_view view = trim(line);
      constexpr std::string_view key = "m_EditorVersion:";
      if (view.substr(0, key.size()) == key)
        return std::string(trim(view.substr(key.size())));
    }
    return {};
  }
  // "6000.6.0f1" -> {6000, 6, 0}
  std::vector<int> versionNumbers(std::string_view version) {
    std::vector<int> numbers;
    int current = -1;
    for (char c : version) {
      if (std::isdigit(static_cast<unsigned char>(c))) {
        current = (current < 0 ? 0 : current * 10) + (c - '0');
      } else {
        if (current >= 0)
          numbers.push_back(current);
        current = -1;
        if (c != '.' && numbers.size() >= 3)
          break;
      }
    }
    if (current >= 0)
      numbers.push_back(current);
    return numbers;
  }
  std::string env(const char *name) {
#ifdef _MSC_VER
    size_t size = 0;
    char *value = nullptr;
    if (_dupenv_s(&value, &size, name) != 0 || !value)
      return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
  }
  // The folder Unity Hub keeps its configuration in.
  fs::path hubConfigFolder() {
#ifdef _WIN32
    std::string appData = env("APPDATA");
    return appData.empty() ? fs::path() : fs::path(appData) / "UnityHub";
#else
    std::string config = env("XDG_CONFIG_HOME");
    if (!config.empty())
      return fs::path(config) / "UnityHub";
    std::string home = env("HOME");
    return home.empty() ? fs::path() : fs::path(home) / ".config" / "UnityHub";
#endif
  }
  std::vector<fs::path> hubEditorRoots() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    std::string programFiles = env("ProgramFiles");
    if (!programFiles.empty())
      roots.push_back(fs::path(programFiles) / "Unity" / "Hub" / "Editor");
#else
    std::string home = env("HOME");
    if (!home.empty())
      roots.push_back(fs::path(home) / "Unity" / "Hub" / "Editor");
#endif
    fs::path hubConfig = hubConfigFolder();
    if (!hubConfig.empty()) {
      if (auto text = readFile(hubConfig / "secondaryInstallPath.json")) {
        try {
          auto json = nlohmann::json::parse(*text);
          if (json.is_string() && !json.get<std::string>().empty())
            roots.push_back(fs::path(json.get<std::string>()));
        } catch (...) {
        }
      }
    }
    return roots;
  }
  fs::path dataFolderFor(const fs::path &path) {
    if (path.empty())
      return {};
    // The editor binary: Unity.exe on Windows, Unity elsewhere. A folder of that name is an install root instead.
    if (path.filename() == "Unity.exe" || (path.filename() == "Unity" && !isDirectory(path))) {
      return path.parent_path() / "Data";
    }
    if (isDirectory(path / "Editor" / "Data"))
      return path / "Editor" / "Data";
    if (isDirectory(path / "Data"))
      return path / "Data";
    return isDirectory(path) ? path : fs::path();
  }
} // namespace
std::shared_ptr<const UnityProject> UnityProject::forFile(const fs::path &file, const fs::path &editorOverride) {
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<const UnityProject>> cache;
  fs::path root = findProjectRoot(file);
  std::string key = pathKey(root) + "|" + pathKey(editorOverride);
  std::lock_guard lock(mutex);
  auto it = cache.find(key);
  if (it != cache.end())
    return it->second;
  auto project = std::shared_ptr<const UnityProject>(new UnityProject(root, editorOverride));
  cache.emplace(key, project);
  return project;
}
UnityProject::UnityProject(fs::path root, const fs::path &editorOverride)
  : root_(std::move(root)) {
  editorVersion_ = root_.empty() ? std::string() : readEditorVersion(root_);
  if (!editorOverride.empty())
    editorData_ = dataFolderFor(editorOverride);
  if (editorData_.empty()) {
    auto roots = hubEditorRoots();
    if (!editorVersion_.empty()) {
      for (const fs::path &hub : roots) {
        if (isDirectory(hub / editorVersion_ / "Editor" / "Data")) {
          editorData_ = hub / editorVersion_ / "Editor" / "Data";
          break;
        }
      }
    }
    if (editorData_.empty()) {
      // Outside a project, or its editor isn't installed: use the newest installed editor.
      std::vector<int> best;
      for (const fs::path &hub : roots) {
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(hub, ec)) {
          if (!isDirectory(entry.path() / "Editor" / "Data"))
            continue;
          auto numbers = versionNumbers(entry.path().filename().string());
          if (numbers > best) {
            best = numbers;
            editorData_ = entry.path() / "Editor" / "Data";
            if (editorVersion_.empty() || root_.empty())
              editorVersion_ = entry.path().filename().string();
          }
        }
      }
    }
  }
  auto numbers = versionNumbers(editorVersion_);
  if (numbers.size() >= 2)
    versionMacro_ = numbers[0] * 100 + numbers[1] * 10;
  if (!root_.empty()) {
    if (auto text = readFile(root_ / "Packages" / "packages-lock.json")) {
      try {
        auto json = nlohmann::json::parse(*text);
        const nlohmann::json dependencies = json.value("dependencies", nlohmann::json::object());
        for (const auto &[name, info] : dependencies.items()) {
          if (!info.is_object() || !info.contains("version") || !info["version"].is_string())
            continue;
          std::string version = info["version"].get<std::string>();
          if (version.rfind("file:", 0) == 0) {
            localPackages_[name] = (root_ / "Packages" / fs::path(version.substr(5))).lexically_normal();
          }
        }
      } catch (...) {
      }
    }
  }
}
fs::path UnityProject::builtinIncludes() const {
  if (editorData_.empty())
    return {};
  if (isDirectory(editorData_ / "Resources" / "CGIncludes"))
    return editorData_ / "Resources" / "CGIncludes";
  if (isDirectory(editorData_ / "CGIncludes"))
    return editorData_ / "CGIncludes";
  return {};
}
fs::path UnityProject::dxcLibrary() const {
  if (editorData_.empty())
    return {};
  fs::path library = editorData_ / "Tools" / std::string(dxcLibraryName());
  return isFile(library) ? library : fs::path();
}
std::optional<fs::path> UnityProject::resolvePackage(const std::string &name) const {
  {
    std::lock_guard lock(cacheMutex_);
    auto it = packageCache_.find(name);
    if (it != packageCache_.end() && isDirectory(it->second))
      return it->second;
  }
  std::optional<fs::path> found;
  if (!root_.empty() && isDirectory(root_ / "Packages" / name)) {
    found = root_ / "Packages" / name;
  } else if (auto local = localPackages_.find(name); local != localPackages_.end() && isDirectory(local->second)) {
    found = local->second;
  } else if (!root_.empty()) {
    std::error_code ec;
    fs::file_time_type newest{};
    for (const auto &entry : fs::directory_iterator(root_ / "Library" / "PackageCache", ec)) {
      std::string dirName = entry.path().filename().string();
      if (dirName.size() > name.size() && dirName.compare(0, name.size(), name) == 0 && dirName[name.size()] == '@') {
        auto time = fs::last_write_time(entry.path(), ec);
        if (!found || time > newest) {
          found = entry.path();
          newest = time;
        }
      }
    }
  }
  if (!found && !editorData_.empty() &&
      isDirectory(editorData_ / "Resources" / "PackageManager" / "BuiltInPackages" / name)) {
    found = editorData_ / "Resources" / "PackageManager" / "BuiltInPackages" / name;
  }
  if (found) {
    std::lock_guard lock(cacheMutex_);
    packageCache_[name] = *found;
  }
  return found;
}
std::optional<fs::path> UnityProject::resolveInclude(std::string_view rawName, const fs::path &includerDir) const {
  std::string name(rawName);
  for (char &c : name) {
    if (c == '\\')
      c = '/';
  }
  if (name.empty())
    return std::nullopt;
  fs::path asPath = fs::path(std::u8string(name.begin(), name.end()));
  if (asPath.is_absolute()) {
    if (isFile(asPath))
      return asPath.lexically_normal();
    return std::nullopt;
  }
  if (name.rfind("Packages/", 0) == 0) {
    size_t slash = name.find('/', 9);
    if (slash != std::string::npos) {
      if (auto package = resolvePackage(name.substr(9, slash - 9))) {
        fs::path candidate = *package / fs::path(name.substr(slash + 1));
        if (isFile(candidate))
          return candidate.lexically_normal();
      }
    }
  }
  std::vector<fs::path> bases{includerDir};
  if (!root_.empty())
    bases.push_back(root_);
  if (fs::path builtin = builtinIncludes(); !builtin.empty())
    bases.push_back(builtin);
  for (const fs::path &base : bases) {
    if (base.empty())
      continue;
    fs::path candidate = base / asPath;
    if (isFile(candidate))
      return candidate.lexically_normal();
  }
  return std::nullopt;
}
} // namespace sls
