#include "analysis/analysis.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include "common/util.h"
#include "shaderlab/parser.h"
#include "shaderlab/reference.h"
namespace fs = std::filesystem;
namespace sls {
DocumentKind documentKindFor(const fs::path &path) {
  std::string extension = toLower(path.extension().string());
  if (extension == ".shader")
    return DocumentKind::ShaderLab;
  if (extension == ".compute")
    return DocumentKind::Compute;
  return DocumentKind::HlslInclude;
}
std::optional<size_t> Analysis::unitAt(size_t offset) const {
  for (size_t i = 0; i < units.size(); ++i) {
    if (units[i].range.containsInclusive(offset))
      return i;
  }
  return std::nullopt;
}
bool Analysis::isProgram(size_t unit) const {
  if (kind == DocumentKind::Compute)
    return true;
  if (kind != DocumentKind::ShaderLab || units[unit].block < 0)
    return false;
  return isProgramBlock(shader.blocks[units[unit].block].kind);
}
std::vector<size_t> Analysis::visibleUnits(size_t unit) const {
  if (kind != DocumentKind::ShaderLab || units[unit].block < 0)
    return {unit};
  const CodeBlock &block = shader.blocks[units[unit].block];
  if (block.kind == BlockKind::GlslProgram)
    return {unit};
  BlockKind includeKind = isCgBlock(block.kind) ? BlockKind::CgInclude : BlockKind::HlslInclude;
  std::vector<size_t> result;
  for (size_t i = 0; i < units.size(); ++i) {
    if (i != unit && shader.blocks[units[i].block].kind == includeKind)
      result.push_back(i);
  }
  result.push_back(unit);
  return result;
}
namespace {
  void addDiagnostic(Analysis &analysis, Span span, Severity severity, std::string message) {
    Diagnostic diagnostic;
    diagnostic.span = span;
    diagnostic.severity = severity;
    diagnostic.message = std::move(message);
    analysis.diagnostics.push_back(std::move(diagnostic));
  }
  bool isStagePragma(std::string_view name) {
    return name == "vertex" || name == "fragment" || name == "geometry" || name == "hull" || name == "domain" ||
           name == "kernel" || name == "surface";
  }
  void validatePragmas(Analysis &analysis, size_t unitIndex) {
    const HlslUnit &unit = analysis.units[unitIndex];
    for (const HlslPragma &pragma : unit.scan.pragmas) {
      const std::string &name = pragma.name;
      bool keyword = ref::isKeywordPragma(name);
      if (!keyword && !ref::find(ref::pragmas(), name)) {
        addDiagnostic(analysis, pragma.nameSpan, Severity::Warning, "Unknown #pragma '" + name + "'.");
        continue;
      }
      if (isStagePragma(name) && pragma.args.empty()) {
        addDiagnostic(analysis, pragma.nameSpan, Severity::Error, "#pragma " + name + " requires a function name.");
      } else if (keyword) {
        if (pragma.args.empty()) {
          addDiagnostic(analysis, pragma.nameSpan, Severity::Error, "#pragma " + name + " requires at least one keyword.");
        }
        std::vector<std::string_view> seen;
        for (const PragmaArg &arg : pragma.args) {
          bool valid = !arg.text.empty() && std::all_of(arg.text.begin(), arg.text.end(), isIdentChar) && !std::isdigit(static_cast<unsigned char>(arg.text[0]));
          if (!valid) {
            addDiagnostic(analysis, arg.span, Severity::Error, "Invalid keyword name '" + arg.text + "'.");
          } else if (std::find(seen.begin(), seen.end(), arg.text) != seen.end() && arg.text.find_first_not_of('_') != std::string::npos) {
            addDiagnostic(analysis, arg.span, Severity::Error, "A keyword set can't include the same keyword twice.");
          }
          seen.push_back(arg.text);
        }
      } else if (name == "target") {
        if (pragma.args.size() != 1 || !ref::find(ref::pragmaTargets(), pragma.args[0].text)) {
          Span span = pragma.args.empty() ? pragma.nameSpan : pragma.args[0].span;
          addDiagnostic(analysis, span, Severity::Warning, "Unknown #pragma target value. Expected one of: 2.0, 2.5, 3.0, 3.5, 4.0, gl4.1, 4.5, 4.6, 5.0.");
        }
      } else if (name == "require") {
        if (pragma.args.empty())
          addDiagnostic(analysis, pragma.nameSpan, Severity::Warning, "#pragma require needs at least one value.");
        for (const PragmaArg &arg : pragma.args) {
          if (!ref::find(ref::pragmaRequires(), arg.text)) {
            addDiagnostic(analysis, arg.span, Severity::Warning, "Unknown #pragma require value '" + arg.text + "'.");
          }
        }
      } else if (name == "only_renderers" || name == "exclude_renderers" || name == "skip_optimizations") {
        if (pragma.args.empty())
          addDiagnostic(analysis, pragma.nameSpan, Severity::Warning, "#pragma " + name + " needs at least one graphics API.");
        for (const PragmaArg &arg : pragma.args) {
          if (!ref::find(ref::renderers(), arg.text)) {
            addDiagnostic(analysis, arg.span, Severity::Warning, "Unknown graphics API '" + arg.text + "'.");
          }
        }
      }
    }
  }
  void checkProgramEntryPoints(Analysis &analysis, size_t unitIndex) {
    bool vertex = false, fragment = false, exempt = false;
    for (size_t visible : analysis.visibleUnits(unitIndex)) {
      for (const HlslPragma &pragma : analysis.units[visible].scan.pragmas) {
        vertex |= pragma.name == "vertex";
        fragment |= pragma.name == "fragment";
        // Surface and ray tracing programs have no vertex/fragment entry points.
        exempt |= pragma.name == "surface" || pragma.name == "raytracing";
      }
      // Entry point pragmas may come from a file included with #include_with_pragmas.
      for (const HlslInclude &include : analysis.units[visible].scan.includes)
        exempt |= include.withPragmas;
    }
    if (exempt || (vertex && fragment))
      return;
    const CodeBlock &block = analysis.shader.blocks[analysis.units[unitIndex].block];
    if (block.pass < 0 && block.subShader >= 0 && !vertex && !fragment)
      return; // SubShader-level blocks are for surface shaders
    std::string missing = !vertex && !fragment ? "#pragma vertex and #pragma fragment" : !vertex ? "#pragma vertex"
                                                                                                 : "#pragma fragment";
    addDiagnostic(analysis, block.keyword, Severity::Warning, "Missing " + missing + "; both are required in regular graphics shaders.");
  }
} // namespace
std::shared_ptr<const Analysis> analyze(fs::path path, std::string text) {
  auto analysis = std::make_shared<Analysis>();
  analysis->kind = documentKindFor(path);
  analysis->path = std::move(path);
  analysis->text = std::move(text);
  analysis->lines = LineIndex(analysis->text);
  if (analysis->kind == DocumentKind::ShaderLab) {
    analysis->shader = parseShaderLab(analysis->text);
    analysis->diagnostics = analysis->shader.diagnostics;
    for (size_t i = 0; i < analysis->shader.blocks.size(); ++i) {
      const CodeBlock &block = analysis->shader.blocks[i];
      HlslUnit unit;
      unit.range = block.content;
      unit.block = static_cast<int>(i);
      if (block.kind != BlockKind::GlslProgram)
        unit.scan = scanHlsl(analysis->text, block.content);
      analysis->units.push_back(std::move(unit));
    }
  } else {
    HlslUnit unit;
    unit.range = {0, analysis->text.size()};
    unit.scan = scanHlsl(analysis->text, unit.range);
    analysis->units.push_back(std::move(unit));
  }
  for (size_t i = 0; i < analysis->units.size(); ++i) {
    if (analysis->kind == DocumentKind::ShaderLab && analysis->shader.blocks[analysis->units[i].block].kind == BlockKind::GlslProgram)
      continue;
    validatePragmas(*analysis, i);
    if (analysis->kind == DocumentKind::ShaderLab && analysis->isProgram(i))
      checkProgramEntryPoints(*analysis, i);
  }
  return analysis;
}
namespace {
  // Blanks the "()" after every use of a zero-parameter macro, skipping comments and strings.
  void blankEmptyMacroParens(std::string &text, const std::set<std::string> &names) {
    size_t i = 0;
    while (i < text.size()) {
      char c = text[i];
      if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
        while (i < text.size() && text[i] != '\n')
          ++i;
      } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
        size_t close = text.find("*/", i + 2);
        i = close == std::string::npos ? text.size() : close + 2;
      } else if (c == '"') {
        ++i;
        while (i < text.size() && text[i] != '"' && text[i] != '\n')
          i += text[i] == '\\' ? 2 : 1;
        ++i;
      } else if (isIdentStart(c)) {
        size_t begin = i;
        while (i < text.size() && isIdentChar(text[i]))
          ++i;
        if (!names.contains(text.substr(begin, i - begin)))
          continue;
        size_t j = i;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t'))
          ++j;
        if (j >= text.size() || text[j] != '(')
          continue;
        size_t k = j + 1;
        while (k < text.size() && (text[k] == ' ' || text[k] == '\t'))
          ++k;
        if (k >= text.size() || text[k] != ')')
          continue;
        text[j] = ' ';
        text[k] = ' ';
        i = k + 1;
      } else {
        ++i;
      }
    }
  }
} // namespace
std::string rewriteForFxc(std::string_view original, bool *pragmaOnce, const std::set<std::string> *zeroParamMacros) {
  // FXC rejects a UTF-8 byte order mark; three spaces keep byte columns unchanged.
  std::string withoutBom;
  std::string_view hlsl = original;
  if (original.substr(0, 3) == "\xEF\xBB\xBF") {
    withoutBom = "   " + std::string(original.substr(3));
    hlsl = withoutBom;
  }
  std::string out;
  out.reserve(hlsl.size());
  size_t start = 0;
  while (start <= hlsl.size()) {
    size_t end = hlsl.find('\n', start);
    bool last = end == std::string_view::npos;
    if (last)
      end = hlsl.size();
    std::string_view line = hlsl.substr(start, end - start);
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      ++i;
    bool handled = false;
    if (i < line.size() && line[i] == '#') {
      size_t j = i + 1;
      while (j < line.size() && (line[j] == ' ' || line[j] == '\t'))
        ++j;
      size_t wordBegin = j;
      while (j < line.size() && isIdentChar(line[j]))
        ++j;
      std::string_view word = line.substr(wordBegin, j - wordBegin);
      if (word == "pragma") {
        size_t k = j;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
          ++k;
        size_t nameBegin = k;
        while (k < line.size() && isIdentChar(line[k]))
          ++k;
        std::string_view name = line.substr(nameBegin, k - nameBegin);
        if (name == "once" && pragmaOnce)
          *pragmaOnce = true;
        if (!ref::isStandardHlslPragma(name)) {
          if (!line.empty() && line.back() == '\r')
            out.push_back('\r');
          handled = true;
        }
      } else if (word == "include_with_pragmas" || word == "define_for_platform_compiler") {
        std::string_view replacement = word == "include_with_pragmas" ? "include" : "define";
        out.append(line.substr(0, wordBegin));
        out.append(replacement);
        out.append(word.size() - replacement.size(), ' ');
        out.append(line.substr(j));
        handled = true;
      }
    }
    if (!handled)
      out.append(line);
    if (last)
      break;
    out.push_back('\n');
    start = end + 1;
  }
  if (zeroParamMacros && !zeroParamMacros->empty())
    blankEmptyMacroParens(out, *zeroParamMacros);
  return out;
}
std::shared_ptr<const CachedFile> loadCachedFile(const fs::path &path) {
  static std::mutex mutex;
  static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const CachedFile>>> cache;
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
  auto file = std::make_shared<CachedFile>();
  file->path = path;
  file->text = std::move(*text);
  file->lines = LineIndex(file->text);
  file->scan = scanHlsl(file->text, {0, file->text.size()});
  bool once = false;
  rewriteForFxc(file->text, &once);
  file->pragmaOnce = once;
  std::lock_guard lock(mutex);
  cache[key] = {time, file};
  return file;
}
} // namespace sls
