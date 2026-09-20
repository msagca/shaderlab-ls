#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "analysis/analysis.h"
#include "compiler/compiler.h"
namespace sls {
enum class CompilerChoice {
  Auto, // FXC, as Unity uses for Direct3D 11, and DXC for `#pragma use_dxc` or where FXC does not exist
  Fxc,
  Dxc,
  None,
};
struct CheckOptions {
  std::filesystem::path editorOverride;
  std::filesystem::path dxcLibrary; // tried before the Unity editor's DXC and the system's
  CompilerChoice compiler = CompilerChoice::Auto;
  std::vector<std::string> keywords; // shader keywords to treat as enabled
  std::vector<ShaderDefine> defines; // extra macros for every compile
};
// The compilers there are for `file`: FXC where it exists, and the first DXC that loads from options.dxcLibrary,
// the project's Unity editor and the system's library path.
struct Compilers {
  const HlslCompiler *fxc = nullptr;
  const HlslCompiler *dxc = nullptr;
};
Compilers compilersFor(const std::filesystem::path &file, const CheckOptions &options);
// Whether options.compiler leaves a compiler to check `file` with.
bool canCompileHlsl(const std::filesystem::path &file, const CheckOptions &options);
// Compiles the document's HLSL programs the way Unity would for Direct3D, with FXC or DXC, and returns the resulting
// diagnostics mapped back onto the document.
std::vector<Diagnostic> checkHlsl(const Analysis &analysis, const CheckOptions &options, const std::function<bool()> &cancelled);
} // namespace sls
