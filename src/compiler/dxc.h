#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "compiler/compiler.h"
namespace sls {
// DXC, loaded at run time from dxcompiler.dll or libdxcompiler.so. Unity compiles with it for Direct3D 12, Vulkan
// and Metal when a shader asks for `#pragma use_dxc`; here it is also the compiler on platforms without FXC.
class Dxc final : public HlslCompiler {
public:
  // The DXC in `library`, loaded once per path. An empty path looks for the library the way the platform finds
  // shared libraries (PATH next to the executable on Windows, LD_LIBRARY_PATH and the system folders elsewhere).
  static const Dxc &load(const std::filesystem::path &library);
  bool available() const override {
    return create_ != nullptr;
  }
  const std::string &loadError() const override {
    return loadError_;
  }
  const std::string &libraryPath() const override {
    return libraryPath_;
  }
  std::string_view name() const override {
    return "dxc";
  }
  std::string profile(std::string_view stage, int model) const override;
  CompileResult compile(const std::string &source, const std::string &sourceName, const std::filesystem::path &sourceDir, const std::vector<ShaderDefine> &defines, const std::string &entry, const std::string &profile, const IncludeOpener &opener) const override;
  CompileResult preprocess(const std::string &source, const std::string &sourceName, const std::filesystem::path &sourceDir, const std::vector<ShaderDefine> &defines, const IncludeOpener &opener) const override;
private:
  explicit Dxc(const std::filesystem::path &library);
  CompileResult run(const std::string &source, const std::string &sourceName, const std::filesystem::path &sourceDir, std::vector<std::string> arguments, const std::vector<ShaderDefine> &defines, const IncludeOpener &opener) const;
  void *create_ = nullptr; // DxcCreateInstance
  std::string loadError_;
  std::string libraryPath_;
};
// The library file name DXC has on this platform.
std::string_view dxcLibraryName();
// Messages in clang's format:
//   C:/path/file.shader:40:34: error: use of undeclared identifier 'foo'
//       return foo;
//              ^
//   file.hlsl:3:1: warning: implicit truncation of vector type [-Wconversion]
//   error: missing entry point definition
std::vector<CompilerMessage> parseDxcMessages(std::string_view output);
} // namespace sls
