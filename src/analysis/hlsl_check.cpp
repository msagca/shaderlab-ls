#include "analysis/hlsl_check.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include "common/util.h"
#include "compiler/dxc.h"
#include "compiler/fxc.h"
#include "shaderlab/reference.h"
#include "unity/project.h"
namespace fs = std::filesystem;
namespace sls {
namespace {
  struct Stage {
    std::string entry;
    std::string prefix; // vs, ps, gs, hs, ds, cs
    std::string define; // SHADER_STAGE_*
    Span argSpan;
  };
  int targetValue(std::string_view text) {
    if (text == "2.0")
      return 20;
    if (text == "2.5")
      return 25;
    if (text == "3.0")
      return 30;
    if (text == "3.5")
      return 35;
    if (text == "4.0")
      return 40;
    if (text == "4.5")
      return 45;
    if (text == "4.6" || text == "gl4.1")
      return 46;
    if (text == "5.0")
      return 50;
    return 0;
  }
  class Checker {
  public:
    Checker(const Analysis &analysis, const CheckOptions &options, Compilers compilers, const std::function<bool()> &cancelled)
      : a_(analysis),
        options_(options),
        compilers_(compilers),
        cancelled_(cancelled),
        project_(UnityProject::forFile(analysis.path, options.editorOverride)),
        docName_(displayPath(analysis.path)),
        docKey_(pathKey(analysis.path)) {
      opener_ = [this](std::string_view name, const fs::path &includerDir) -> std::optional<IncludeFile> {
        auto resolved = project_->resolveInclude(name, includerDir);
        if (!resolved)
          return std::nullopt;
        auto file = loadCachedFile(*resolved);
        if (!file)
          return std::nullopt;
        auto &rewritten = rewritten_[pathKey(file->path)];
        if (!rewritten)
          rewritten = std::make_shared<const std::string>(rewriteForFxc(file->text, nullptr, &zeroParamMacros_));
        return IncludeFile{file->path, rewritten, file->pragmaOnce};
      };
    }
    std::vector<Diagnostic> run() {
      if (a_.kind == DocumentKind::Glsl)
        return {}; // GLSL is not compiled, in a block or in a file of its own
      collectZeroParamMacros();
      if (a_.kind == DocumentKind::HlslInclude) {
        compiler_ = choose(false);
        if (!compiler_)
          return {};
        std::string source = "#line 1 \"" + docName_ + "\"\n" + rewriteForFxc(a_.text, nullptr, &zeroParamMacros_);
        CompileResult result = compiler_->preprocess(source, docName_, a_.path.parent_path(), baseDefines(25), opener_);
        for (const CompilerMessage &message : result.messages)
          map(message, result, 0, nullptr);
      } else {
        for (size_t unit = 0; unit < a_.units.size(); ++unit) {
          if (cancelled_())
            break;
          if (a_.isProgram(unit))
            checkProgram(unit);
        }
      }
      std::set<std::tuple<size_t, size_t, std::string>> seen;
      std::vector<Diagnostic> unique;
      for (Diagnostic &diagnostic : out_) {
        if (seen.insert({diagnostic.span.begin, diagnostic.span.end, diagnostic.message}).second) {
          unique.push_back(std::move(diagnostic));
        }
      }
      return unique;
    }
  private:
    // FXC for Unity's default Direct3D 11 target, DXC for shaders that ask for it (Unity then compiles Direct3D 12
    // with it) or where there is no FXC.
    const HlslCompiler *choose(bool useDxc) const {
      switch (options_.compiler) {
      case CompilerChoice::Fxc:
        return compilers_.fxc;
      case CompilerChoice::Dxc:
        return compilers_.dxc;
      case CompilerChoice::None:
        return nullptr;
      case CompilerChoice::Auto:
        break;
      }
      if (useDxc && compilers_.dxc)
        return compilers_.dxc;
      return compilers_.fxc ? compilers_.fxc : compilers_.dxc;
    }
    // FXC rejects NAME() macros, so find every one reachable from this document before compiling.
    void collectZeroParamMacros() {
      auto addFrom = [&](const HlslScan &scan) {
        for (const HlslDecl &decl : scan.decls) {
          if (decl.kind == DeclKind::Macro && decl.zeroParams)
            zeroParamMacros_.insert(decl.name);
        }
      };
      std::vector<std::pair<std::string, fs::path>> queue;
      for (const HlslUnit &unit : a_.units) {
        addFrom(unit.scan);
        for (const HlslInclude &include : unit.scan.includes)
          queue.emplace_back(include.path, a_.path.parent_path());
      }
      queue.emplace_back("HLSLSupport.cginc", a_.path.parent_path());
      queue.emplace_back("UnityShaderVariables.cginc", a_.path.parent_path());
      std::set<std::string> visited;
      while (!queue.empty() && visited.size() < 800) {
        auto [name, dir] = queue.back();
        queue.pop_back();
        auto resolved = project_->resolveInclude(name, dir);
        if (!resolved || !visited.insert(pathKey(*resolved)).second)
          continue;
        auto file = loadCachedFile(*resolved);
        if (!file)
          continue;
        addFrom(file->scan);
        for (const HlslInclude &include : file->scan.includes)
          queue.emplace_back(include.path, resolved->parent_path());
      }
    }
    std::vector<ShaderDefine> baseDefines(int target) const {
      std::vector<ShaderDefine> defines{{"SHADER_API_D3D11", "1"}, {"SHADER_API_DESKTOP", "1"}, {"SHADER_TARGET", std::to_string(target)}};
      if (project_->versionMacro() > 0)
        defines.push_back({"UNITY_VERSION", std::to_string(project_->versionMacro())});
      defines.insert(defines.end(), options_.defines.begin(), options_.defines.end());
      return defines;
    }
    Span anchorFor(size_t unit) const {
      if (a_.kind == DocumentKind::ShaderLab)
        return a_.shader.blocks[a_.units[unit].block].keyword;
      return {0, 0};
    }
    void checkProgram(size_t unit) {
      std::vector<size_t> visible = a_.visibleUnits(unit);
      std::vector<const HlslPragma *> pragmas;
      for (size_t v : visible) {
        for (const HlslPragma &pragma : a_.units[v].scan.pragmas)
          pragmas.push_back(&pragma);
      }
      bool compute = a_.kind == DocumentKind::Compute;
      int target = compute ? 50 : 25;
      bool surface = false;
      bool requiresSm5 = false;
      bool rayQuery = false;
      bool useDxc = false;
      bool neverDxc = false;
      std::vector<Stage> stages;
      std::vector<ShaderDefine> keywordDefines;
      std::vector<std::string> dynamicKeywords;
      auto has = [](const HlslPragma &pragma, std::initializer_list<std::string_view> names) {
        return std::any_of(pragma.args.begin(), pragma.args.end(), [&](const PragmaArg &arg) {
          return std::find(names.begin(), names.end(), arg.text) != names.end();
        });
      };
      for (const HlslPragma *pragma : pragmas) {
        const std::string &name = pragma->name;
        if (name == "only_renderers" && !has(*pragma, {"d3d11", "dx11"}))
          return;
        if (name == "exclude_renderers" && has(*pragma, {"d3d11", "dx11"}))
          return;
        if (name == "surface")
          surface = true;
        if (name == "target" && !pragma->args.empty() && targetValue(pragma->args[0].text) > 0) {
          target = targetValue(pragma->args[0].text);
        }
        // Features that only shader model 5 provides raise the compile target, as Unity does.
        if (name == "require" && has(*pragma, {"compute", "randomwrite", "uav", "tesshw", "tessellation", "msaatex", "cubearray", "interpolators32", "mrt8", "sparsetex", "setrtarrayindexfromanyshader", "inlineraytracing"})) {
          requiresSm5 = true;
        }
        if (name == "require" && has(*pragma, {"inlineraytracing"}))
          rayQuery = true;
        // Unity compiles Direct3D 12 with DXC when asked to; with no API list the request covers every API.
        if (name == "use_dxc" && (pragma->args.empty() || has(*pragma, {"d3d11", "d3d12"})))
          useDxc = true;
        if (name == "never_use_dxc")
          neverDxc = true;
        if (!pragma->args.empty()) {
          const PragmaArg &arg = pragma->args[0];
          if (!compute) {
            if (name == "vertex")
              stages.push_back({arg.text, "vs", "SHADER_STAGE_VERTEX", arg.span});
            if (name == "fragment")
              stages.push_back({arg.text, "ps", "SHADER_STAGE_FRAGMENT", arg.span});
            if (name == "geometry")
              stages.push_back({arg.text, "gs", "SHADER_STAGE_GEOMETRY", arg.span});
            if (name == "hull")
              stages.push_back({arg.text, "hs", "SHADER_STAGE_HULL", arg.span});
            if (name == "domain")
              stages.push_back({arg.text, "ds", "SHADER_STAGE_DOMAIN", arg.span});
          } else if (name == "kernel") {
            stages.push_back({arg.text, "cs", "SHADER_STAGE_COMPUTE", arg.span});
          }
        }
        if (ref::isKeywordPragma(name))
          selectKeywords(*pragma, keywordDefines, dynamicKeywords);
      }
      compiler_ = choose(useDxc && !neverDxc);
      if (!compiler_)
        return;
      std::string source;
      if (a_.kind == DocumentKind::ShaderLab && isCgBlock(a_.shader.blocks[a_.units[unit].block].kind)) {
        if (project_->builtinIncludes().empty()) {
          add(anchorFor(unit), Severity::Information,
            "Unity editor not found, so the files CGPROGRAM includes automatically (HLSLSupport.cginc, "
            "UnityShaderVariables.cginc) are unavailable. Set initializationOptions.unityEditorPath.");
        } else {
          source += "#include \"HLSLSupport.cginc\"\n#include \"UnityShaderVariables.cginc\"\n";
        }
      }
      for (const std::string &keyword : dynamicKeywords)
        source += "uniform bool " + keyword + ";\n";
      for (size_t v : visible) {
        Span range = a_.units[v].range;
        size_t line = a_.lines.lineOf(range.begin);
        source += "#line " + std::to_string(line + 1) + " \"" + docName_ + "\"\n";
        source.append(range.begin - a_.lines.lineStart(line), ' ');
        source += rewriteForFxc(std::string_view(a_.text).substr(range.begin, range.end - range.begin), nullptr, &zeroParamMacros_);
        source += "\n";
      }
      std::vector<ShaderDefine> defines = baseDefines(target);
      defines.insert(defines.end(), keywordDefines.begin(), keywordDefines.end());
      if (auto pass = passDefine(unit))
        defines.push_back({*pass, "1"});
      // HLSLSupport.cginc swaps DX9-style samplers (sampler2D, tex2D), which DXC no longer accepts, for its own.
      if (compiler_->name() == "dxc")
        defines.push_back({"UNITY_COMPILER_DXC", "1"});
      const std::string sourceName = "shaderlab-ls-preamble.hlsl";
      if (stages.empty() || surface) {
        CompileResult result = compiler_->preprocess(source, sourceName, a_.path.parent_path(), defines, opener_);
        for (const CompilerMessage &message : result.messages)
          map(message, result, unit, nullptr);
        return;
      }
      for (const Stage &stage : stages) {
        if (cancelled_())
          return;
        std::vector<ShaderDefine> stageDefines = defines;
        stageDefines.push_back({stage.define, "1"});
        bool sm5 = target >= 45 || requiresSm5 || stage.prefix == "hs" || stage.prefix == "ds" || stage.prefix == "cs";
        std::string profile = compiler_->profile(stage.prefix, rayQuery ? 65 : sm5 ? 50
                                                                                   : 40);
        CompileResult result = compiler_->compile(source, sourceName, a_.path.parent_path(), stageDefines, stage.entry, profile, opener_);
        for (const CompilerMessage &message : result.messages)
          map(message, result, unit, &stage);
      }
    }
    // Built-in Render Pipeline passes get UNITY_PASS_<LIGHTMODE> (for example UNITY_PASS_META), which the built-in
    // include files test for.
    std::optional<std::string> passDefine(size_t unit) const {
      if (a_.kind != DocumentKind::ShaderLab)
        return std::nullopt;
      const CodeBlock &block = a_.shader.blocks[a_.units[unit].block];
      if (block.subShader < 0 || block.pass < 0)
        return std::nullopt;
      for (const Tag &tag : a_.shader.subShaders[block.subShader].passes[block.pass].tags) {
        if (!iequals(tag.key, "LightMode"))
          continue;
        const ref::Entry *entry = ref::find(ref::tagValues("LightMode"), tag.value);
        if (!entry || entry->detail.find("Built-in") == std::string_view::npos)
          return std::nullopt;
        std::string define = "UNITY_PASS_";
        for (char c : entry->name)
          define.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        return define;
      }
      return std::nullopt;
    }
    // Unity's default variant: the first keyword of each set, unless the set allows "all off".
    void selectKeywords(const HlslPragma &pragma, std::vector<ShaderDefine> &defines, std::vector<std::string> &dynamic) {
      std::vector<std::string> keywords;
      for (const PragmaArg &arg : pragma.args)
        keywords.push_back(arg.text);
      if (keywords.empty())
        return;
      auto isNone = [](const std::string &k) { return k.find_first_not_of('_') == std::string::npos; };
      if (pragma.name.rfind("dynamic_branch", 0) == 0) {
        for (const std::string &k : keywords) {
          if (!isNone(k))
            dynamic.push_back(k);
        }
        return;
      }
      bool chosen = false;
      for (const std::string &k : keywords) {
        if (std::find(options_.keywords.begin(), options_.keywords.end(), k) != options_.keywords.end()) {
          defines.push_back({k, "1"});
          chosen = true;
        }
      }
      if (chosen)
        return;
      bool singleFeature = pragma.name.rfind("shader_feature", 0) == 0 && keywords.size() == 1;
      if (!isNone(keywords[0]) && !singleFeature)
        defines.push_back({keywords[0], "1"});
    }
    void add(Span span, Severity severity, std::string message, std::string code = {}, std::optional<RelatedLocation> related = std::nullopt) {
      Diagnostic diagnostic;
      diagnostic.span = span;
      diagnostic.severity = severity;
      diagnostic.message = std::move(message);
      diagnostic.code = std::move(code);
      diagnostic.source = std::string(compiler_ ? compiler_->name() : "fxc");
      diagnostic.related = std::move(related);
      out_.push_back(std::move(diagnostic));
    }
    bool isDocument(const std::string &file) const {
      return file == docName_ || pathKey(fs::path(file)) == docKey_;
    }
    Span documentSpan(const CompilerMessage &message) const {
      size_t line = static_cast<size_t>(std::max(message.line - 1, 0));
      if (line >= a_.lines.lineCount())
        line = a_.lines.lineCount() - 1;
      size_t start = a_.lines.lineStart(line);
      size_t end = a_.lines.lineEnd(a_.text, line);
      size_t begin = std::min(start + static_cast<size_t>(std::max(message.column - 1, 0)), end);
      size_t stop = message.columnEnd > 0 ? std::min(start + static_cast<size_t>(message.columnEnd), end) : begin;
      if (stop <= begin) {
        stop = begin;
        while (stop < end && isIdentChar(a_.text[stop]))
          ++stop;
        if (stop == begin && stop < end)
          ++stop;
      }
      return {begin, stop};
    }
    // Does `file` (transitively) include the file with key `target`?
    bool reaches(const fs::path &file, const std::string &target, std::set<std::string> &visited) const {
      std::string key = pathKey(file);
      if (key == target)
        return true;
      if (!visited.insert(key).second || visited.size() > 600)
        return false;
      auto cached = loadCachedFile(file);
      if (!cached)
        return false;
      for (const HlslInclude &include : cached->scan.includes) {
        auto resolved = project_->resolveInclude(include.path, file.parent_path());
        if (resolved && reaches(*resolved, target, visited))
          return true;
      }
      return false;
    }
    void map(const CompilerMessage &message, const CompileResult &result, size_t unit, const Stage *stage) {
      Severity severity = message.warning ? Severity::Warning : Severity::Error;
      if (message.file.empty()) {
        // FXC's X3501 and DXC's "missing entry point definition" belong on the #pragma that names the entry point.
        bool noEntry = message.code == "X3501" || message.text.find("missing entry point") != std::string::npos;
        Span span = stage && noEntry ? stage->argSpan : anchorFor(unit);
        add(span, severity, message.text, message.code);
        return;
      }
      if (isDocument(message.file)) {
        add(documentSpan(message), severity, message.text, message.code);
        return;
      }
      if (message.warning)
        return; // Warnings inside Unity's shader libraries are noise here.
      std::optional<fs::path> resolved;
      if (auto it = result.includes.find(message.file); it != result.includes.end())
        resolved = it->second;
      Span anchor = anchorFor(unit);
      if (resolved) {
        std::string target = pathKey(*resolved);
        bool found = false;
        for (size_t v : a_.visibleUnits(unit)) {
          for (const HlslInclude &include : a_.units[v].scan.includes) {
            auto direct = project_->resolveInclude(include.path, a_.path.parent_path());
            std::set<std::string> visited;
            if (direct && reaches(*direct, target, visited)) {
              anchor = include.pathSpan;
              found = true;
              break;
            }
          }
          if (found)
            break;
        }
      }
      std::string where = resolved ? displayPath(*resolved) : message.file;
      std::optional<RelatedLocation> related;
      if (resolved)
        related = RelatedLocation{displayPath(*resolved), message.line - 1, message.column - 1, message.text};
      add(anchor, severity, "In " + where + "(" + std::to_string(message.line) + "," + std::to_string(message.column) + "): " + message.text, message.code, related);
    }
    const Analysis &a_;
    const CheckOptions &options_;
    Compilers compilers_;
    const HlslCompiler *compiler_ = nullptr; // the one the program or file being checked uses
    const std::function<bool()> &cancelled_;
    std::shared_ptr<const UnityProject> project_;
    std::string docName_;
    std::string docKey_;
    IncludeOpener opener_;
    std::set<std::string> zeroParamMacros_;
    std::map<std::string, std::shared_ptr<const std::string>> rewritten_;
    std::vector<Diagnostic> out_;
  };
} // namespace
Compilers compilersFor(const fs::path &file, const CheckOptions &options) {
  Compilers compilers;
  if (Fxc::instance().available())
    compilers.fxc = &Fxc::instance();
  std::vector<fs::path> libraries;
  if (!options.dxcLibrary.empty())
    libraries.push_back(options.dxcLibrary);
  if (fs::path editor = UnityProject::forFile(file, options.editorOverride)->dxcLibrary(); !editor.empty()) {
    libraries.push_back(editor);
  }
  libraries.emplace_back(); // the system's
  for (const fs::path &library : libraries) {
    const Dxc &dxc = Dxc::load(library);
    if (dxc.available()) {
      compilers.dxc = &dxc;
      break;
    }
  }
  return compilers;
}
bool canCompileHlsl(const fs::path &file, const CheckOptions &options) {
  if (options.compiler == CompilerChoice::None)
    return false;
  Compilers compilers = compilersFor(file, options);
  switch (options.compiler) {
  case CompilerChoice::Fxc:
    return compilers.fxc != nullptr;
  case CompilerChoice::Dxc:
    return compilers.dxc != nullptr;
  default:
    return compilers.fxc || compilers.dxc;
  }
}
std::vector<Diagnostic> checkHlsl(const Analysis &analysis, const CheckOptions &options, const std::function<bool()> &cancelled) {
  if (options.compiler == CompilerChoice::None)
    return {};
  Compilers compilers = compilersFor(analysis.path, options);
  if (!compilers.fxc && !compilers.dxc)
    return {};
  return Checker(analysis, options, compilers, cancelled).run();
}
} // namespace sls
