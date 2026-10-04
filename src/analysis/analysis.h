#pragma once
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/text.h"
#include "analysis/variant.h"
#include "compiler/compiler.h"
#include "hlsl/scanner.h"
#include "shaderlab/model.h"
namespace sls {
enum class DocumentKind {
  ShaderLab, // .shader
  Compute, // .compute: one HLSL program with #pragma kernel entry points
  HlslInclude, // .hlsl, .cginc, .hlslinc: shared code without a program context
  Glsl, // .glsl, .glslinc: formatted, never analyzed, like a GLSLPROGRAM block
};
DocumentKind documentKindFor(const std::filesystem::path &path);
// A region of HLSL: a ShaderLab code block, or the whole file for HLSL documents. GLSL units carry no scan.
struct HlslUnit {
  Span range;
  int block = -1; // index into ShaderFile::blocks, -1 for HLSL documents
  HlslScan scan;
  std::vector<Span> inactive; // code an #if leaves out of the variant analyzed
};
struct Analysis;
// The variant a document is analyzed as, which decides what its #if directives leave out.
struct AnalyzeOptions {
  VariantChoice variant;
  std::vector<ShaderDefine> defines; // the `defines` setting
  std::filesystem::path editorOverride;
  // For an include file: the programs that include it, which its #if directives are worked out in.
  std::vector<std::shared_ptr<const Analysis>> includers;
};
struct CachedFile;
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
  // GLSL is formatted but not analyzed: a GLSLPROGRAM block, or all of a .glsl/.glslinc document.
  bool isGlsl(size_t unit) const;
  // includedFiles() of this analysis, kept for a moment: hover, completion and the rest each ask for the same files.
  struct IncludeMemo {
    std::mutex mutex;
    std::map<std::pair<std::vector<size_t>, std::string>, std::pair<std::chrono::steady_clock::time_point, std::vector<std::shared_ptr<const CachedFile>>>> entries;
  };
  std::shared_ptr<IncludeMemo> includeMemo = std::make_shared<IncludeMemo>();
};
std::shared_ptr<const Analysis> analyze(std::filesystem::path path, std::string text, const AnalyzeOptions &options = {});
// Include files read from disk, cached by modification time.
struct CachedFile {
  std::filesystem::path path;
  std::string text;
  LineIndex lines;
  HlslScan scan;
  bool pragmaOnce = false;
};
std::shared_ptr<const CachedFile> loadCachedFile(const std::filesystem::path &path);
// The files the code of `units` includes, directly or not, in the order they are reached; CGPROGRAM code also gets
// the two files Unity includes in it. At most 400.
std::vector<std::shared_ptr<const CachedFile>> includedFiles(const Analysis &analysis, const std::vector<size_t> &units, const std::filesystem::path &editorOverride);
// Rewrites code that Unity's shader preprocessor accepts but FXC's does not, keeping line and column positions:
// - Unity #pragma lines are blanked;
// - #include_with_pragmas and #define_for_platform_compiler become #include and #define;
// - macros declared with an empty parameter list, NAME(), become object-like, and so do their call sites.
std::string rewriteForFxc(std::string_view hlsl, bool *pragmaOnce = nullptr, const std::set<std::string> *zeroParamMacros = nullptr);
} // namespace sls
