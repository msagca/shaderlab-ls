#include "analysis/analysis.h"
#include "analysis/conditionals.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <deque>
#include <cctype>
#include <map>
#include <mutex>
#include "common/util.h"
#include "shaderlab/parser.h"
#include "shaderlab/reference.h"
#include "unity/project.h"
namespace fs = std::filesystem;
namespace sls {
DocumentKind documentKindFor(const fs::path &path) {
  std::string extension = toLower(path.extension().string());
  if (extension == ".shader")
    return DocumentKind::ShaderLab;
  if (extension == ".compute")
    return DocumentKind::Compute;
  if (extension == ".glsl" || extension == ".glslinc")
    return DocumentKind::Glsl;
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
bool Analysis::isGlsl(size_t unit) const {
  if (kind == DocumentKind::Glsl)
    return true;
  if (kind != DocumentKind::ShaderLab || units[unit].block < 0)
    return false;
  return shader.blocks[units[unit].block].kind == BlockKind::GlslProgram;
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
  std::optional<long long> defineValue(const std::string &text) {
    char *end = nullptr;
    long long value = std::strtoll(text.c_str(), &end, 0);
    return !text.empty() && end == text.c_str() + text.size() ? std::optional(value) : std::nullopt;
  }
  int targetOf(std::string_view text) {
    static const std::pair<std::string_view, int> kTargets[] = {{"2.0", 20}, {"2.5", 25}, {"3.0", 30}, {"3.5", 35}, {"4.0", 40}, {"4.5", 45}, {"4.6", 46}, {"gl4.1", 46}, {"5.0", 50}};
    for (auto [name, value] : kTargets) {
      if (text == name)
        return value;
    }
    return 0;
  }
  // What the compile of unit `u` defines before its code: Direct3D 11, the target, the Unity version, the `defines`
  // setting and the variant's keywords. A name only an include file or Unity itself might define is not known either
  // way.
  MacroState unitState(const Analysis &a, size_t u, const AnalyzeOptions &options, const VariantChoice &variant, std::function<bool(std::string_view)> elsewhere) {
    bool program = a.kind == DocumentKind::Compute || a.isProgram(u);
    MacroState state;
    state.closed = a.kind != DocumentKind::HlslInclude;
    state.elsewhere = std::move(elsewhere);
    int target = a.kind == DocumentKind::Compute ? 50 : 25;
    // The program's pragmas, and those of the files it includes with #include_with_pragmas, whose keyword sets are
    // its own as much as the ones it writes out.
    std::vector<const HlslPragma *> pragmas;
    std::vector<std::shared_ptr<const CachedFile>> pragmaFiles;
    auto project = UnityProject::forFile(a.path, options.editorOverride);
    for (size_t v : a.visibleUnits(u)) {
      for (const HlslPragma &pragma : a.units[v].scan.pragmas)
        pragmas.push_back(&pragma);
      for (const HlslInclude &include : a.units[v].scan.includes) {
        auto resolved = include.withPragmas ? project->resolveInclude(include.path, a.path.parent_path()) : std::nullopt;
        if (auto file = resolved ? loadCachedFile(*resolved) : nullptr) {
          for (const HlslPragma &pragma : file->scan.pragmas)
            pragmas.push_back(&pragma);
          pragmaFiles.push_back(std::move(file));
        }
      }
    }
    {
      for (const HlslPragma *pragmaPointer : pragmas) {
        const HlslPragma &pragma = *pragmaPointer;
        if (pragma.name == "target" && !pragma.args.empty() && targetOf(pragma.args[0].text) > 0)
          target = targetOf(pragma.args[0].text);
        if (!ref::isKeywordPragma(pragma.name))
          continue;
        // A program knows its variant; an include block is compiled into every program, each with its own.
        std::vector<std::string> enabled = program ? enabledKeywords(pragma, variant) : std::vector<std::string>();
        for (const PragmaArg &arg : pragma.args) {
          if (isNoKeyword(arg.text))
            continue;
          if (std::find(enabled.begin(), enabled.end(), arg.text) != enabled.end())
            state.defined[arg.text] = 1;
          else if (program)
            state.undefined.insert(arg.text);
        }
      }
    }
    if (!program && a.kind == DocumentKind::ShaderLab) {
      for (const HlslUnit &other : a.units) {
        for (const HlslPragma &pragma : other.scan.pragmas) {
          for (const PragmaArg &arg : ref::isKeywordPragma(pragma.name) ? pragma.args : std::vector<PragmaArg>())
            state.unknown.insert(arg.text);
        }
      }
    }
    state.defined["SHADER_API_D3D11"] = 1;
    state.defined["SHADER_API_DESKTOP"] = 1;
    // An include block's target is each program's, which may raise it.
    if (program)
      state.defined["SHADER_TARGET"] = target;
    else
      state.unknown.insert("SHADER_TARGET");
    int versionMacro = UnityProject::forFile(a.path, options.editorOverride)->versionMacro();
    if (versionMacro > 0)
      state.defined["UNITY_VERSION"] = versionMacro;
    for (const ShaderDefine &define : options.defines)
      state.defined[define.name] = defineValue(define.value);
    return state;
  }
  // Names Unity or an include file of `a` may define, which a condition can't be sure of either way.
  std::function<bool(std::string_view)> definedElsewhere(const Analysis &a, const fs::path &editorOverride) {
    auto included = std::make_shared<std::optional<std::set<std::string, std::less<>>>>();
    return [&a, editorOverride, included](std::string_view name) {
      if (name.starts_with("__") || name.starts_with("UNITY_") || name.starts_with("SHADER_STAGE_"))
        return true;
      if (!*included) {
        included->emplace();
        std::vector<size_t> code;
        for (size_t u = 0; u < a.units.size(); ++u) {
          if (!a.isGlsl(u))
            code.push_back(u);
        }
        for (const auto &file : includedFiles(a, code, editorOverride)) {
          for (const HlslDecl &decl : file->scan.decls) {
            if (decl.kind == DeclKind::Macro)
              (*included)->insert(decl.name);
          }
        }
      }
      return (*included)->contains(name);
    };
  }
  // The code in both lists.
  std::vector<Span> overlap(const std::vector<Span> &x, const std::vector<Span> &y) {
    std::vector<Span> result;
    size_t i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
      size_t begin = std::max(x[i].begin, y[j].begin), end = std::min(x[i].end, y[j].end);
      if (begin < end)
        result.push_back({begin, end});
      (x[i].end < y[j].end ? i : j)++;
    }
    return result;
  }
  // An include file's code that no includer compiles: worked out once for each, in the program that includes it, with
  // that program's variant and what it defines before the #include. Nothing when an includer is not a program that
  // includes it directly, in which case its conditions are not known either way.
  std::optional<std::vector<Span>> inactiveInIncluders(const Analysis &a, const AnalyzeOptions &options, std::string &names) {
    std::optional<std::vector<Span>> common;
    std::string key = pathKey(a.path);
    for (const std::shared_ptr<const Analysis> &includer : options.includers) {
      const Analysis &program = *includer;
      auto project = UnityProject::forFile(program.path, options.editorOverride);
      // The program and the unit of it whose #include brings this file in.
      std::optional<std::tuple<size_t, size_t, Span>> site;
      for (size_t u = 0; u < program.units.size() && !site; ++u) {
        if (program.isGlsl(u) || !(program.kind == DocumentKind::Compute || program.isProgram(u)))
          continue;
        for (size_t v : program.visibleUnits(u)) {
          for (const HlslInclude &include : program.units[v].scan.includes) {
            auto resolved = project->resolveInclude(include.path, program.path.parent_path());
            if (!site && resolved && pathKey(*resolved) == key)
              site = {u, v, include.span};
          }
        }
      }
      if (!site)
        return std::nullopt;
      auto [u, v, include] = *site;
      MacroState state = unitState(program, u, options, {{}, options.variant.global}, definedElsewhere(program, options.editorOverride));
      // What the program defines before the #include, in the blocks before it and in its own up to it.
      bool compiled = true;
      for (size_t w : program.visibleUnits(u)) {
        if (w != v) {
          inactiveRegions(program.text, program.units[w].range, state);
          continue;
        }
        std::vector<Span> before = inactiveRegions(program.text, {program.units[v].range.begin, include.begin}, state);
        compiled = before.empty() || before.back().end < include.begin;
        break;
      }
      if (!compiled)
        continue; // the variant does not include it at all, and says nothing about its code
      std::vector<Span> regions = inactiveRegions(a.text, a.units[0].range, state);
      common = common ? overlap(*common, regions) : regions;
      names += (names.empty() ? "" : ", ") + program.path.filename().string();
    }
    return common;
  }
  // The code each unit's #if directives leave out of the variant analyzed, worked out with what the compile defines.
  // An include file is worked out in the programs that include it when there are any, and with what it defines
  // itself when not: a condition on anything else is then not known either way.
  void findInactiveCode(Analysis &a, const AnalyzeOptions &options) {
    auto elsewhere = definedElsewhere(a, options.editorOverride);
    for (size_t u = 0; u < a.units.size(); ++u) {
      if (a.isGlsl(u))
        continue;
      bool program = a.kind == DocumentKind::Compute || a.isProgram(u);
      HlslUnit &unit = a.units[u];
      std::string message;
      std::string includers;
      std::optional<std::vector<Span>> inIncluders;
      if (a.kind == DocumentKind::HlslInclude && !options.includers.empty())
        inIncluders = inactiveInIncluders(a, options, includers);
      if (inIncluders) {
        unit.inactive = std::move(*inIncluders);
        message = "Not compiled where it is included, in the variants checked of " + includers + ".";
      } else {
        MacroState state = unitState(a, u, options, options.variant, elsewhere);
        // The include blocks a program sees come before it, as Unity compiles them.
        std::vector<size_t> visible = a.visibleUnits(u);
        for (size_t v : visible) {
          if (v != u)
            inactiveRegions(a.text, a.units[v].range, state);
        }
        unit.inactive = inactiveRegions(a.text, unit.range, state);
        std::string variant;
        for (size_t v : visible) {
          for (const HlslPragma &pragma : a.units[v].scan.pragmas) {
            for (const std::string &keyword : program ? enabledKeywords(pragma, options.variant) : std::vector<std::string>())
              variant += (variant.empty() ? "" : ", ") + keyword;
          }
        }
        message = program ? "Not compiled in the variant checked (" + (variant.empty() ? std::string("no keywords") : "keywords: " + variant) + ")."
                          : "Not compiled: an #if leaves it out.";
      }
      for (HlslDecl &decl : unit.scan.decls) {
        decl.active = std::none_of(unit.inactive.begin(), unit.inactive.end(), [&](Span region) {
          return decl.nameSpan.begin >= region.begin && decl.nameSpan.begin < region.end;
        });
      }
      for (Span region : unit.inactive) {
        // Up to the end of its last line, so that it ends on text rather than at the start of the line after.
        Span span = region;
        while (span.end > span.begin && (a.text[span.end - 1] == '\n' || a.text[span.end - 1] == '\r'))
          --span.end;
        if (span.empty())
          continue;
        Diagnostic diagnostic;
        diagnostic.span = span;
        diagnostic.severity = Severity::Hint;
        diagnostic.unnecessary = true;
        diagnostic.message = message;
        a.diagnostics.push_back(std::move(diagnostic));
      }
    }
  }
} // namespace
std::shared_ptr<const Analysis> analyze(fs::path path, std::string text, const AnalyzeOptions &options) {
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
    // A GLSL document is one unit so that positions inside it resolve, but nothing reads it as HLSL.
    if (analysis->kind != DocumentKind::Glsl)
      unit.scan = scanHlsl(analysis->text, unit.range);
    analysis->units.push_back(std::move(unit));
  }
  for (size_t i = 0; i < analysis->units.size(); ++i) {
    if (analysis->isGlsl(i))
      continue;
    validatePragmas(*analysis, i);
    if (analysis->kind == DocumentKind::ShaderLab && analysis->isProgram(i))
      checkProgramEntryPoints(*analysis, i);
  }
  findInactiveCode(*analysis, options);
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
namespace {
  // How long what was read from disk is trusted before the file is looked at again.
  constexpr auto kFileRecheck = std::chrono::seconds(2);
} // namespace
std::vector<std::shared_ptr<const CachedFile>> followIncludes(const Analysis &a, const std::vector<size_t> &units, const fs::path &editorOverride);
std::shared_ptr<const CachedFile> loadCachedFile(const fs::path &path) {
  struct Entry {
    fs::file_time_type time;
    std::chrono::steady_clock::time_point checked;
    std::shared_ptr<const CachedFile> file;
  };
  static std::mutex mutex;
  static std::map<std::string, Entry> cache;
  std::string key = pathKey(path);
  auto now = std::chrono::steady_clock::now();
  {
    // A file checked a moment ago is not checked again: one request reads hundreds of include files, and asking the
    // file system about each of them every time costs more than everything else the request does.
    std::lock_guard lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end() && now - it->second.checked < kFileRecheck)
      return it->second.file;
  }
  std::error_code ec;
  auto time = fs::last_write_time(path, ec);
  if (ec)
    return nullptr;
  {
    std::lock_guard lock(mutex);
    auto it = cache.find(key);
    if (it != cache.end() && it->second.time == time) {
      it->second.checked = now;
      return it->second.file;
    }
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
  cache[key] = {time, now, file};
  return file;
}
std::vector<std::shared_ptr<const CachedFile>> includedFiles(const Analysis &a, const std::vector<size_t> &units, const fs::path &editorOverride) {
  auto now = std::chrono::steady_clock::now();
  auto memoKey = std::pair(units, pathKey(editorOverride));
  {
    std::lock_guard lock(a.includeMemo->mutex);
    auto it = a.includeMemo->entries.find(memoKey);
    if (it != a.includeMemo->entries.end() && now - it->second.first < kFileRecheck)
      return it->second.second;
  }
  std::vector<std::shared_ptr<const CachedFile>> files = followIncludes(a, units, editorOverride);
  std::lock_guard lock(a.includeMemo->mutex);
  a.includeMemo->entries[memoKey] = {now, files};
  return files;
}
std::vector<std::shared_ptr<const CachedFile>> followIncludes(const Analysis &a, const std::vector<size_t> &units, const fs::path &editorOverride) {
  auto project = UnityProject::forFile(a.path, editorOverride);
  std::vector<std::shared_ptr<const CachedFile>> files;
  std::set<std::string> visited;
  std::deque<std::pair<std::string, fs::path>> queue;
  for (size_t unit : units) {
    for (const HlslInclude &include : a.units[unit].scan.includes)
      queue.emplace_back(include.path, a.path.parent_path());
  }
  if (a.kind == DocumentKind::ShaderLab) {
    for (size_t unit : units) {
      if (a.units[unit].block >= 0 && isCgBlock(a.shader.blocks[a.units[unit].block].kind)) {
        queue.emplace_back("HLSLSupport.cginc", a.path.parent_path());
        queue.emplace_back("UnityShaderVariables.cginc", a.path.parent_path());
        break;
      }
    }
  }
  while (!queue.empty() && files.size() < 400) {
    auto [name, dir] = queue.front();
    queue.pop_front();
    auto resolved = project->resolveInclude(name, dir);
    if (!resolved || !visited.insert(pathKey(*resolved)).second)
      continue;
    auto file = loadCachedFile(*resolved);
    if (!file)
      continue;
    for (const HlslInclude &include : file->scan.includes)
      queue.emplace_back(include.path, resolved->parent_path());
    files.push_back(std::move(file));
  }
  return files;
}
} // namespace sls
