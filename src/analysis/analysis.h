#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/text.h"
#include "hlsl/scanner.h"
#include "shaderlab/model.h"
namespace sls {
enum class DocumentKind {
  ShaderLab, // .shader
  Compute, // .compute: one HLSL program with #pragma kernel entry points
  HlslInclude, // .hlsl, .cginc, .hlslinc: shared code without a program context
};
DocumentKind documentKindFor(const std::filesystem::path &path);
// A region of HLSL: a ShaderLab code block, or the whole file for HLSL documents.
struct HlslUnit {
  Span range;
  int block = -1; // index into ShaderFile::blocks, -1 for HLSL documents
  HlslScan scan;
};
struct Analysis {
  DocumentKind kind = DocumentKind::ShaderLab;
  std::filesystem::path path;
  std::string text;
  LineIndex lines;
  ShaderFile shader; // ShaderLab documents only
  std::vector<HlslUnit> units;
  std::vector<Diagnostic> diagnostics; // syntax and directive checks; FXC results are separate
  std::optional<size_t> unitAt(size_t offset) const;
  // Units whose code is visible from `unit`: for a program block, the matching include blocks plus itself.
  std::vector<size_t> visibleUnits(size_t unit) const;
  bool isProgram(size_t unit) const;
};
std::shared_ptr<const Analysis> analyze(std::filesystem::path path, std::string text);
// Include files read from disk, cached by modification time.
struct CachedFile {
  std::filesystem::path path;
  std::string text;
  LineIndex lines;
  HlslScan scan;
  bool pragmaOnce = false;
};
std::shared_ptr<const CachedFile> loadCachedFile(const std::filesystem::path &path);
// Rewrites code that Unity's shader preprocessor accepts but FXC's does not, keeping line and column positions:
// - Unity #pragma lines are blanked;
// - #include_with_pragmas and #define_for_platform_compiler become #include and #define;
// - macros declared with an empty parameter list, NAME(), become object-like, and so do their call sites.
std::string rewriteForFxc(std::string_view hlsl, bool *pragmaOnce = nullptr, const std::set<std::string> *zeroParamMacros = nullptr);
} // namespace sls
