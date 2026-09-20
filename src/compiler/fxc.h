#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "compiler/compiler.h"
namespace sls {
// Thin wrapper over D3DCompile / D3DPreprocess from d3dcompiler_47.dll, loaded at runtime. The compiler Unity uses
// for its default Direct3D 11 target; it exists on Windows only.
class Fxc final : public HlslCompiler {
public:
  static Fxc &instance();
  bool available() const override {
    return compile_ != nullptr;
  }
  const std::string &loadError() const override {
    return loadError_;
  }
  const std::string &libraryPath() const override {
    return libraryPath_;
  }
  std::string_view name() const override {
    return "fxc";
  }
  std::string profile(std::string_view stage, int model) const override;
  CompileResult compile(const std::string &source, const std::string &sourceName, const std::filesystem::path &sourceDir, const std::vector<ShaderDefine> &defines, const std::string &entry, const std::string &profile, const IncludeOpener &opener) const override;
  CompileResult preprocess(const std::string &source, const std::string &sourceName, const std::filesystem::path &sourceDir, const std::vector<ShaderDefine> &defines, const IncludeOpener &opener) const override;
private:
  Fxc();
  void *compile_ = nullptr;
  void *preprocess_ = nullptr;
  std::string loadError_;
  std::string libraryPath_;
};
std::vector<CompilerMessage> parseFxcMessages(std::string_view output);
} // namespace sls
