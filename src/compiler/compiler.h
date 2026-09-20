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
struct CompilerMessage {
  std::string file; // as the compiler reports it; empty when the message has no location
  int line = 0; // 1-based
  int column = 0; // 1-based
  int columnEnd = 0; // 1-based, inclusive; 0 if not reported
  bool warning = false;
  std::string code; // FXC's X3004, DXC's -Wconversion
  std::string text;
};
struct ShaderDefine {
  std::string name;
  std::string value;
};
struct IncludeFile {
  std::filesystem::path path;
  std::shared_ptr<const std::string> text;
  bool pragmaOnce = false;
};
// Resolves and loads an #include. `includerDir` is the directory of the including file.
using IncludeOpener =
  std::function<std::optional<IncludeFile>(std::string_view name, const std::filesystem::path &includerDir)>;
struct CompileResult {
  bool success = false;
  std::vector<CompilerMessage> messages;
  // Include names as the compiler reports them -> resolved paths.
  std::map<std::string, std::filesystem::path> includes;
};
// An HLSL compiler loaded at run time: FXC (d3dcompiler_47.dll, Windows only) or DXC (dxcompiler.dll,
// libdxcompiler.so).
class HlslCompiler {
public:
  virtual ~HlslCompiler() = default;
  virtual bool available() const = 0;
  // Why the compiler is not available, when it is not.
  virtual const std::string &loadError() const = 0;
  // The library it was loaded from.
  virtual const std::string &libraryPath() const = 0;
  // "fxc" or "dxc", the source of the diagnostics it produces.
  virtual std::string_view name() const = 0;
  // The profile for `stage` (vs, ps, gs, hs, ds, cs) at shader model `model` (40, 50, 60, 65, ...), as far as this
  // compiler supports it: FXC stops at 5.0 and DXC starts at 6.0.
  virtual std::string profile(std::string_view stage, int model) const = 0;
  virtual CompileResult compile(const std::string &source, const std::string &sourceName, const std::filesystem::path &sourceDir, const std::vector<ShaderDefine> &defines, const std::string &entry, const std::string &profile, const IncludeOpener &opener) const = 0;
  virtual CompileResult preprocess(const std::string &source, const std::string &sourceName, const std::filesystem::path &sourceDir, const std::vector<ShaderDefine> &defines, const IncludeOpener &opener) const = 0;
};
} // namespace sls
