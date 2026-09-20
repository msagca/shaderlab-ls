#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
namespace sls {
// Everything needed to resolve includes the way the Unity Editor does for one project.
class UnityProject {
public:
  // `editorOverride` may point at the Editor folder, its Data folder, or Unity.exe.
  static std::shared_ptr<const UnityProject> forFile(const std::filesystem::path &file,
    const std::filesystem::path &editorOverride);
  const std::filesystem::path &root() const {
    return root_;
  } // empty outside a project
  const std::filesystem::path &editorData() const {
    return editorData_;
  } // empty if not found
  const std::string &editorVersion() const {
    return editorVersion_;
  }
  // UNITY_VERSION macro value, e.g. 6000.6 -> 600060.
  int versionMacro() const {
    return versionMacro_;
  }
  std::filesystem::path builtinIncludes() const;
  // The DXC library the editor ships, which Unity compiles `#pragma use_dxc` shaders with. Empty if there is none.
  std::filesystem::path dxcLibrary() const;
  std::optional<std::filesystem::path> resolveInclude(std::string_view name,
    const std::filesystem::path &includerDir) const;
private:
  UnityProject(std::filesystem::path root, const std::filesystem::path &editorOverride);
  std::optional<std::filesystem::path> resolvePackage(const std::string &name) const;
  std::filesystem::path root_;
  std::filesystem::path editorData_;
  std::string editorVersion_;
  int versionMacro_ = 0;
  std::map<std::string, std::filesystem::path> localPackages_;
  mutable std::mutex cacheMutex_;
  mutable std::map<std::string, std::filesystem::path> packageCache_;
};
} // namespace sls
