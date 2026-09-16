#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sls {

struct FxcMessage {
  std::string file;  // as reported by FXC; empty when the message has no location
  int line = 0;  // 1-based
  int column = 0;  // 1-based
  int columnEnd = 0;  // 1-based, inclusive; 0 if not reported
  bool warning = false;
  std::string code;  // X3004
  std::string text;
};

struct FxcDefine {
  std::string name;
  std::string value;
};

struct FxcIncludeFile {
  std::filesystem::path path;
  std::shared_ptr<const std::string> text;
  bool pragmaOnce = false;
};

// Resolves and loads an #include. `includerDir` is the directory of the including file.
using FxcIncludeOpener =
    std::function<std::optional<FxcIncludeFile>(std::string_view name, const std::filesystem::path& includerDir)>;

struct FxcResult {
  bool success = false;
  std::vector<FxcMessage> messages;
  // Include names as FXC reports them -> resolved paths.
  std::map<std::string, std::filesystem::path> includes;
};

// Thin wrapper over D3DCompile / D3DPreprocess from d3dcompiler_47.dll, loaded at runtime.
class Fxc {
 public:
  static Fxc& instance();

  bool available() const { return compile_ != nullptr; }
  const std::string& loadError() const { return loadError_; }
  const std::string& dllPath() const { return dllPath_; }

  FxcResult compile(const std::string& source, const std::string& sourceName, const std::filesystem::path& sourceDir,
                    const std::vector<FxcDefine>& defines, const std::string& entry, const std::string& profile,
                    const FxcIncludeOpener& opener) const;
  FxcResult preprocess(const std::string& source, const std::string& sourceName,
                       const std::filesystem::path& sourceDir, const std::vector<FxcDefine>& defines,
                       const FxcIncludeOpener& opener) const;

 private:
  Fxc();
  void* compile_ = nullptr;
  void* preprocess_ = nullptr;
  std::string loadError_;
  std::string dllPath_;
};

std::vector<FxcMessage> parseFxcMessages(std::string_view output);

}  // namespace sls
