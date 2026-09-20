#include "lsp/features.h"
#include <algorithm>
#include <cctype>
#include <deque>
#include <set>
#include "common/util.h"
#include "hlsl/builtins.h"
#include "shaderlab/lexer.h"
#include "shaderlab/reference.h"
#include "unity/project.h"
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace sls {
namespace {
  // LSP CompletionItemKind
  enum ItemKind {
    kItemFunction = 3,
    kItemField = 5,
    kItemVariable = 6,
    kItemClass = 7,
    kItemModule = 9,
    kItemProperty = 10,
    kItemValue = 12,
    kItemKeyword = 14,
    kItemSnippet = 15,
    kItemConstant = 21,
    kItemStruct = 22,
  };
  // LSP SymbolKind
  enum SymbolKind {
    kSymModule = 2,
    kSymNamespace = 3,
    kSymPackage = 4,
    kSymClass = 5,
    kSymProperty = 7,
    kSymField = 8,
    kSymFunction = 12,
    kSymVariable = 13,
    kSymConstant = 14,
    kSymObject = 19,
    kSymStruct = 23,
  };
  json markdown(std::string value) {
    return {{"kind", "markdown"}, {"value", std::move(value)}};
  }
  std::string entryMarkdown(const ref::Entry &entry, std::string_view language = "shaderlab") {
    std::string text = "```" + std::string(language) + "\n" + std::string(entry.detail) + "\n```";
    if (!entry.doc.empty())
      text += "\n\n" + std::string(entry.doc);
    return text;
  }
  std::string valueMarkdown(const ref::Entry &entry) {
    return "**" + std::string(entry.name) + "** (" + std::string(entry.detail) + ")\n\n" + std::string(entry.doc);
  }
  std::string spanText(const Analysis &a, Span span) {
    return a.text.substr(span.begin, span.end - span.begin);
  }
  std::string propertyMarkdown(const Analysis &a, const MaterialProperty &property) {
    return "```shaderlab\n" + spanText(a, property.span) + "\n```\n\nMaterial property (" + property.type + ")";
  }
  Span wordAt(std::string_view text, size_t offset) {
    size_t begin = std::min(offset, text.size());
    size_t end = begin;
    while (begin > 0 && isIdentChar(text[begin - 1]))
      --begin;
    while (end < text.size() && isIdentChar(text[end]))
      ++end;
    return {begin, end};
  }
  bool inside(Span span, size_t offset) {
    return offset >= span.begin && offset <= span.end && !span.empty();
  }
  json location(const fs::path &path, std::string_view text, const LineIndex &lines, Span span, Encoding encoding) {
    return {{"uri", pathToUri(path)}, {"range", toRange(text, lines, span, encoding)}};
  }
  // ---- declarations across units and includes --------------------------------
  struct ExternalDecl {
    std::shared_ptr<const CachedFile> file;
    const HlslDecl *decl;
  };
  std::vector<std::shared_ptr<const CachedFile>> includedFiles(const Analysis &a, const std::vector<size_t> &units, const FeatureContext &context) {
    auto project = UnityProject::forFile(a.path, context.editorOverride);
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
  int declPriority(DeclKind kind) {
    switch (kind) {
    case DeclKind::Function:
      return 0;
    case DeclKind::Struct:
      return 1;
    case DeclKind::CBuffer:
      return 2;
    case DeclKind::Variable:
      return 3;
    case DeclKind::Macro:
      return 4;
    case DeclKind::Field:
      return 5;
    }
    return 6;
  }
  const HlslDecl *bestDecl(const std::vector<const HlslDecl *> &candidates) {
    const HlslDecl *best = nullptr;
    for (const HlslDecl *decl : candidates) {
      if (!best || declPriority(decl->kind) < declPriority(best->kind))
        best = decl;
    }
    return best;
  }
  const HlslDecl *findLocalDecl(const Analysis &a, const std::vector<size_t> &units, std::string_view name) {
    std::vector<const HlslDecl *> candidates;
    for (size_t unit : units) {
      for (const HlslDecl &decl : a.units[unit].scan.decls) {
        if (decl.name == name)
          candidates.push_back(&decl);
      }
    }
    return bestDecl(candidates);
  }
  std::optional<ExternalDecl> findExternalDecl(const std::vector<std::shared_ptr<const CachedFile>> &files, std::string_view name) {
    std::optional<ExternalDecl> best;
    for (const auto &file : files) {
      for (const HlslDecl &decl : file->scan.decls) {
        if (decl.name != name || decl.kind == DeclKind::Field)
          continue;
        if (!best || declPriority(decl.kind) < declPriority(best->decl->kind))
          best = ExternalDecl{file, &decl};
      }
    }
    return best;
  }
  int itemKindFor(DeclKind kind) {
    switch (kind) {
    case DeclKind::Function:
      return kItemFunction;
    case DeclKind::Struct:
      return kItemStruct;
    case DeclKind::CBuffer:
      return kItemModule;
    case DeclKind::Variable:
      return kItemVariable;
    case DeclKind::Field:
      return kItemField;
    case DeclKind::Macro:
      return kItemConstant;
    }
    return kItemVariable;
  }
  std::string declMarkdown(const HlslDecl &decl, std::string_view where = {}) {
    std::string text = "```hlsl\n" + decl.detail + "\n```";
    if (!decl.container.empty())
      text += "\n\nIn `" + decl.container + "`";
    if (!where.empty())
      text += "\n\nDefined in `" + std::string(where) + "`";
    return text;
  }
  // ---- completion helpers ------------------------------------------------------
  class Items {
  public:
    explicit Items(const FeatureContext &context)
      : context_(context) {}
    void add(std::string label, int kind, std::string detail = {}, std::string documentation = {}, std::string insertText = {}, bool snippet = false) {
      if (!labels_.insert(label).second)
        return;
      json item{{"label", label}, {"kind", kind}};
      if (!detail.empty())
        item["detail"] = std::move(detail);
      if (!documentation.empty())
        item["documentation"] = markdown(std::move(documentation));
      if (!insertText.empty()) {
        if (snippet && !context_.snippets)
          return addPlain(std::move(item), insertText);
        item["insertText"] = std::move(insertText);
        if (snippet)
          item["insertTextFormat"] = 2;
      }
      items_.push_back(std::move(item));
    }
    void addEntries(std::span<const ref::Entry> table, int kind, bool useSnippets = true) {
      for (const ref::Entry &entry : table) {
        bool snippet = useSnippets && !entry.snippet.empty();
        add(std::string(entry.name), kind, std::string(entry.detail), entryMarkdown(entry), snippet ? std::string(entry.snippet) : std::string(), snippet);
      }
    }
    void addValues(std::span<const ref::Entry> table, int kind = kItemValue, std::string_view quote = {}) {
      for (const ref::Entry &entry : table) {
        std::string insert = quote.empty() ? std::string() : std::string(quote) + std::string(entry.name) + std::string(quote);
        add(std::string(entry.name), kind, std::string(entry.detail), valueMarkdown(entry), insert);
      }
    }
    json result() {
      return {{"isIncomplete", false}, {"items", std::move(items_)}};
    }
  private:
    void addPlain(json item, const std::string &snippetText) {
      // Strip ${n:placeholder} / $n markers for clients without snippet support.
      std::string plain;
      for (size_t i = 0; i < snippetText.size(); ++i) {
        if (snippetText[i] == '$' && i + 1 < snippetText.size()) {
          if (snippetText[i + 1] == '{') {
            size_t colon = snippetText.find(':', i);
            size_t close = snippetText.find('}', i);
            if (colon != std::string::npos && close != std::string::npos && colon < close)
              plain += snippetText.substr(colon + 1, close - colon - 1);
            i = close == std::string::npos ? snippetText.size() : close;
            continue;
          }
          if (std::isdigit(static_cast<unsigned char>(snippetText[i + 1]))) {
            ++i;
            continue;
          }
        }
        plain.push_back(snippetText[i]);
      }
      item["insertText"] = plain;
      items_.push_back(std::move(item));
    }
    const FeatureContext &context_;
    std::set<std::string> labels_;
    json items_ = json::array();
  };
  void addDecl(Items &items, const HlslDecl &decl, std::string_view where = {}) {
    if (decl.kind == DeclKind::Field)
      return;
    items.add(decl.name, itemKindFor(decl.kind), decl.detail, declMarkdown(decl, where));
  }
  void addFunctions(Items &items, const Analysis &a, const std::vector<size_t> &units, const FeatureContext &context) {
    for (size_t unit : units) {
      for (const HlslDecl &decl : a.units[unit].scan.decls) {
        if (decl.kind == DeclKind::Function)
          addDecl(items, decl);
      }
    }
    for (const auto &file : includedFiles(a, units, context)) {
      for (const HlslDecl &decl : file->scan.decls) {
        if (decl.kind == DeclKind::Function)
          addDecl(items, decl, file->path.filename().string());
      }
    }
  }
  json hlslCompletion(const Analysis &a, size_t unit, size_t offset, const FeatureContext &context) {
    Items items(context);
    std::vector<size_t> units = a.visibleUnits(unit);
    size_t lineStart = a.lines.lineStart(a.lines.lineOf(offset));
    std::string_view prefix = std::string_view(a.text).substr(lineStart, offset - lineStart);
    std::string_view trimmed = prefix;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
      trimmed.remove_prefix(1);
    if (!trimmed.empty() && trimmed.front() == '#') {
      std::string_view after = trimmed.substr(1);
      while (!after.empty() && (after.front() == ' ' || after.front() == '\t'))
        after.remove_prefix(1);
      size_t wordEnd = 0;
      while (wordEnd < after.size() && isIdentChar(after[wordEnd]))
        ++wordEnd;
      if (wordEnd == after.size()) {
        items.addValues(ref::preprocessorDirectives(), kItemKeyword);
        return items.result();
      }
      if (after.substr(0, wordEnd) != "pragma")
        return items.result();
      std::vector<std::string_view> words;
      std::string_view rest = after.substr(wordEnd);
      size_t i = 0;
      while (i < rest.size()) {
        while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i])))
          ++i;
        size_t begin = i;
        while (i < rest.size() && !std::isspace(static_cast<unsigned char>(rest[i])))
          ++i;
        if (i > begin)
          words.push_back(rest.substr(begin, i - begin));
      }
      bool trailingSpace = !rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()));
      size_t complete = trailingSpace ? words.size() : (words.empty() ? 0 : words.size() - 1);
      if (rest.empty())
        return items.result();
      if (complete == 0) {
        items.addEntries(ref::pragmas(), kItemKeyword);
        return items.result();
      }
      std::string_view pragma = words[0];
      if (complete == 1 && (pragma == "vertex" || pragma == "fragment" || pragma == "geometry" || pragma == "hull" ||
                             pragma == "domain" || pragma == "kernel" || pragma == "surface")) {
        addFunctions(items, a, units, context);
      } else if (complete == 1 && pragma == "target") {
        items.addValues(ref::pragmaTargets());
      } else if (pragma == "require") {
        items.addValues(ref::pragmaRequires());
      } else if (pragma == "only_renderers" || pragma == "exclude_renderers" || pragma == "skip_optimizations" ||
                 pragma == "use_dxc" || pragma == "never_use_dxc") {
        items.addValues(ref::renderers());
      }
      return items.result();
    }
    for (size_t u : units) {
      for (const HlslDecl &decl : a.units[u].scan.decls)
        addDecl(items, decl);
    }
    if (a.kind == DocumentKind::ShaderLab) {
      for (const MaterialProperty &property : a.shader.properties) {
        items.add(property.name, kItemProperty, "Material property (" + property.type + ")", propertyMarkdown(a, property));
      }
    }
    for (const std::string &keyword : hlsl::keywords())
      items.add(keyword, kItemKeyword);
    for (const std::string &type : hlsl::types())
      items.add(type, kItemClass);
    for (const std::string &function : hlsl::intrinsics())
      items.add(function, kItemFunction, "HLSL intrinsic function");
    for (const auto &file : includedFiles(a, units, context)) {
      std::string where = file->path.filename().string();
      for (const HlslDecl &decl : file->scan.decls)
        addDecl(items, decl, where);
    }
    return items.result();
  }
  const Scope *scopeAt(const Analysis &a, size_t offset) {
    const Scope *best = nullptr;
    for (const Scope &scope : a.shader.scopes) {
      // A scope ends just past its '}'; an unclosed scope runs to the end of the file.
      bool closed = scope.span.end > 0 && a.text[scope.span.end - 1] == '}';
      bool contains = offset > scope.span.begin && (offset < scope.span.end || (!closed && offset <= scope.span.end));
      if (contains && (!best || scope.span.begin >= best->span.begin))
        best = &scope;
    }
    return best;
  }
  void addPropertyRefs(Items &items, const Analysis &a, bool bracketed) {
    for (const MaterialProperty &property : a.shader.properties) {
      std::string label = bracketed ? property.name : "[" + property.name + "]";
      items.add(label, kItemProperty, "Material property (" + property.type + ")", propertyMarkdown(a, property));
    }
  }
  json shaderLabCompletion(const Analysis &a, size_t offset, const FeatureContext &context) {
    Items items(context);
    size_t lineStart = a.lines.lineStart(a.lines.lineOf(offset));
    std::string prefix = a.text.substr(lineStart, offset - lineStart);
    bool inString = std::count(prefix.begin(), prefix.end(), '"') % 2 == 1;
    std::string beforeString = inString ? prefix.substr(0, prefix.rfind('"')) : prefix;
    std::string head = beforeString;
    if (!inString) {
      while (!head.empty() && isIdentChar(head.back()))
        head.pop_back();
    }
    std::vector<Token> tokens = lexShaderLab(head);
    tokens.pop_back(); // End
    bool afterBracket = !head.empty() && trim(head).size() > 0 && trim(head).back() == '[';
    const Scope *scope = scopeAt(a, offset);
    if (!scope) {
      if (!a.shader.hasShader) {
        items.add("Shader", kItemSnippet, "Shader \"<name>\" { ... }", "Defines a Shader object with a given name.", "Shader \"${1:Custom/NewShader}\"\n{\n\t$0\n}", true);
      }
      return items.result();
    }
    auto commandArgs = [&]() {
      if (tokens.empty() || tokens[0].kind != Tok::Ident)
        return;
      std::string_view command = tokens[0].text;
      if (afterBracket)
        return addPropertyRefs(items, a, true);
      size_t valueCount = static_cast<size_t>(std::count_if(tokens.begin() + 1, tokens.end(), [](const Token &t) {
        return t.kind == Tok::Ident || t.kind == Tok::Number || t.kind == Tok::RBracket;
      }));
      if (auto values = ref::commandValues(command); !values.empty() && !iequals(command, "BlendOp")) {
        if (valueCount == 0)
          items.addValues(values);
      } else if (iequals(command, "BlendOp")) {
        items.addValues(ref::blendOperations());
      } else if (iequals(command, "Blend")) {
        if (valueCount == 0)
          items.add("Off", kItemValue, "state", "Disables blending.");
        items.addValues(ref::blendFactors());
      } else if (iequals(command, "ColorMask")) {
        if (valueCount == 0)
          items.addValues(ref::colorMaskValues());
      } else if (iequals(command, "Fallback")) {
        if (valueCount == 0)
          items.add("Off", kItemValue, "Fallback Off", "Do not use a fallback Shader object.");
        return;
      } else if (!iequals(command, "Offset")) {
        return;
      }
      addPropertyRefs(items, a, false);
    };
    switch (scope->kind) {
    case ScopeKind::Shader:
      if (tokens.empty()) {
        items.addEntries(ref::shaderBlockKeywords(), kItemKeyword);
      } else {
        commandArgs();
      }
      break;
    case ScopeKind::SubShader:
    case ScopeKind::Pass:
      if (tokens.empty()) {
        items.addEntries(scope->kind == ScopeKind::Pass ? ref::passBlockKeywords() : ref::subShaderBlockKeywords(), kItemKeyword);
        items.addEntries(ref::commands(), kItemKeyword);
      } else {
        commandArgs();
      }
      break;
    case ScopeKind::Stencil:
      if (tokens.empty()) {
        items.addEntries(ref::stencilFields(), kItemKeyword);
      } else if (afterBracket) {
        addPropertyRefs(items, a, true);
      } else if (tokens.size() == 1 && tokens[0].kind == Tok::Ident) {
        items.addValues(ref::stencilFieldValues(tokens[0].text));
        addPropertyRefs(items, a, false);
      }
      break;
    case ScopeKind::Tags: {
      bool valuePosition = !tokens.empty() && tokens.back().kind == Tok::Equals;
      std::string_view quote = inString ? "" : "\"";
      if (valuePosition) {
        std::string key = tokens.size() >= 2 && tokens[tokens.size() - 2].kind == Tok::String ? stringValue(tokens[tokens.size() - 2]) : "";
        items.addValues(ref::tagValues(key), kItemValue, quote);
      } else {
        items.addValues(scope->parent == ScopeKind::Pass ? ref::passTags() : ref::subShaderTags(), kItemProperty, quote);
      }
      break;
    }
    case ScopeKind::Properties: {
      size_t open = prefix.rfind('[');
      size_t close = prefix.rfind(']');
      if (open != std::string::npos && (close == std::string::npos || close < open)) {
        items.addEntries(ref::propertyAttributes(), kItemKeyword);
      } else if (tokens.size() >= 4 && tokens[0].kind == Tok::Ident && tokens[1].kind == Tok::LParen &&
                 tokens[2].kind == Tok::String && tokens[3].kind == Tok::Comma && tokens.size() == 4) {
        items.addEntries(ref::propertyTypes(), kItemClass);
      } else if (tokens.size() >= 5 && tokens.back().kind == Tok::Comma && tokens[tokens.size() - 2].kind == Tok::String) {
        items.addEntries(ref::propertyTypes(), kItemClass);
      }
      break;
    }
    default:
      break;
    }
    return items.result();
  }
  // ---- hover helpers -----------------------------------------------------------
  json hoverResult(const Analysis &a, Span span, std::string value, const FeatureContext &context) {
    return {{"contents", markdown(std::move(value))}, {"range", toRange(a.text, a.lines, span, context.encoding)}};
  }
  std::span<const ref::Entry> tableForCommandArg(const Command &command, bool stencilField) {
    if (stencilField)
      return ref::stencilFieldValues(command.name);
    if (auto values = ref::commandValues(command.name); !values.empty())
      return values;
    if (iequals(command.name, "Blend"))
      return ref::blendFactors();
    if (iequals(command.name, "ColorMask"))
      return ref::colorMaskValues();
    return {};
  }
  std::vector<std::pair<const Command *, bool>> allCommands(const ShaderFile &shader) {
    std::vector<std::pair<const Command *, bool>> result;
    auto addList = [&](const std::vector<Command> &commands) {
      for (const Command &command : commands) {
        result.emplace_back(&command, false);
        for (const Command &child : command.children)
          result.emplace_back(&child, true);
      }
    };
    for (const SubShader &subShader : shader.subShaders) {
      addList(subShader.commands);
      for (const Pass &pass : subShader.passes)
        addList(pass.commands);
    }
    return result;
  }
  std::vector<std::pair<const std::vector<Tag> *, bool>> allTags(const ShaderFile &shader) {
    std::vector<std::pair<const std::vector<Tag> *, bool>> result;
    for (const SubShader &subShader : shader.subShaders) {
      result.emplace_back(&subShader.tags, false);
      for (const Pass &pass : subShader.passes)
        result.emplace_back(&pass.tags, true);
    }
    return result;
  }
  json shaderLabHover(const Analysis &a, size_t offset, const FeatureContext &context) {
    const ShaderFile &shader = a.shader;
    for (const MaterialProperty &property : shader.properties) {
      for (const PropertyAttribute &attribute : property.attributes) {
        if (inside(attribute.nameSpan, offset)) {
          if (const ref::Entry *entry = ref::find(ref::propertyAttributes(), attribute.name)) {
            return hoverResult(a, attribute.nameSpan, entryMarkdown(*entry), context);
          }
          return hoverResult(a, attribute.nameSpan, "Custom MaterialPropertyDrawer `" + attribute.name + "`", context);
        }
      }
      if (inside(property.typeSpan, offset)) {
        if (const ref::Entry *entry = ref::find(ref::propertyTypes(), property.type)) {
          return hoverResult(a, property.typeSpan, entryMarkdown(*entry), context);
        }
      }
      if (inside(property.nameSpan, offset))
        return hoverResult(a, property.nameSpan, propertyMarkdown(a, property), context);
    }
    for (auto [command, stencilField] : allCommands(shader)) {
      if (inside(command->nameSpan, offset)) {
        const ref::Entry *entry = stencilField ? ref::find(ref::stencilFields(), command->name) : ref::findCommand(command->name);
        if (!entry)
          return nullptr;
        std::string text = entryMarkdown(*entry);
        if (!stencilField && !iequals(command->name, "Stencil")) {
          text +=
            "\n\nUse it in a `Pass` block to set the render state for that Pass, or in a `SubShader` block to set "
            "the render state for all Passes in that SubShader.";
        }
        return hoverResult(a, command->nameSpan, text, context);
      }
      for (const CommandArg &arg : command->args) {
        if (arg.comma || !inside(arg.span, offset))
          continue;
        if (arg.propertyRef) {
          if (const MaterialProperty *property = shader.findProperty(arg.text)) {
            return hoverResult(a, arg.span, propertyMarkdown(a, *property), context);
          }
          return nullptr;
        }
        auto table = tableForCommandArg(*command, stencilField);
        if (iequals(command->name, "Blend") && iequals(arg.text, "Off")) {
          return hoverResult(a, arg.span, "**Off** (state)\n\nDisables blending.", context);
        }
        if (const ref::Entry *entry = ref::find(table, arg.text))
          return hoverResult(a, arg.span, valueMarkdown(*entry), context);
        return nullptr;
      }
    }
    for (auto [tags, isPass] : allTags(shader)) {
      for (const Tag &tag : *tags) {
        if (inside(tag.keySpan, offset)) {
          if (const ref::Entry *entry = ref::find(isPass ? ref::passTags() : ref::subShaderTags(), tag.key)) {
            return hoverResult(a, tag.keySpan, entryMarkdown(*entry), context);
          }
          return nullptr;
        }
        if (inside(tag.valueSpan, offset)) {
          if (const ref::Entry *entry = ref::find(ref::tagValues(tag.key), tag.value)) {
            return hoverResult(a, tag.valueSpan, valueMarkdown(*entry), context);
          }
          return nullptr;
        }
      }
    }
    for (const CodeBlock &block : shader.blocks) {
      if (inside(block.keyword, offset)) {
        if (const ref::Entry *entry = ref::find(ref::subShaderBlockKeywords(), blockKeyword(block.kind))) {
          return hoverResult(a, block.keyword, entryMarkdown(*entry), context);
        }
      }
    }
    Span word = wordAt(a.text, offset);
    if (word.empty())
      return nullptr;
    std::string name = spanText(a, word);
    if (iequals(name, "Shader")) {
      return hoverResult(a, word,
        "```shaderlab\nShader \"<name>\" { <optional: Material properties> <One or more SubShader "
        "definitions> <optional: custom editor> <optional: fallback> }\n```\n\nDefines a Shader object "
        "with a given name.",
        context);
    }
    if (const ref::Entry *entry = ref::findKeywordAnywhere(name))
      return hoverResult(a, word, entryMarkdown(*entry), context);
    return nullptr;
  }
  const HlslPragma *pragmaAt(const Analysis &a, size_t unit, size_t offset) {
    for (const HlslPragma &pragma : a.units[unit].scan.pragmas) {
      if (inside(pragma.span, offset))
        return &pragma;
    }
    return nullptr;
  }
  bool isStagePragma(std::string_view name) {
    return name == "vertex" || name == "fragment" || name == "geometry" || name == "hull" || name == "domain" ||
           name == "kernel" || name == "surface";
  }
  json hlslHover(const Analysis &a, size_t unit, size_t offset, const FeatureContext &context) {
    std::vector<size_t> units = a.visibleUnits(unit);
    if (const HlslPragma *pragma = pragmaAt(a, unit, offset)) {
      if (inside(pragma->nameSpan, offset)) {
        if (const ref::Entry *entry = ref::find(ref::pragmas(), pragma->name)) {
          return hoverResult(a, pragma->nameSpan, entryMarkdown(*entry, "hlsl"), context);
        }
        if (ref::isKeywordPragma(pragma->name)) {
          std::string base = pragma->name.rfind("multi_compile", 0) == 0    ? "multi_compile"
                             : pragma->name.rfind("shader_feature", 0) == 0 ? "shader_feature"
                                                                            : "dynamic_branch";
          if (const ref::Entry *entry = ref::find(ref::pragmas(), base)) {
            return hoverResult(a, pragma->nameSpan, entryMarkdown(*entry, "hlsl"), context);
          }
        }
        return nullptr;
      }
      for (const PragmaArg &arg : pragma->args) {
        if (!inside(arg.span, offset))
          continue;
        std::span<const ref::Entry> table;
        if (pragma->name == "target")
          table = ref::pragmaTargets();
        if (pragma->name == "require")
          table = ref::pragmaRequires();
        if (pragma->name == "only_renderers" || pragma->name == "exclude_renderers" || pragma->name == "skip_optimizations")
          table = ref::renderers();
        if (const ref::Entry *entry = ref::find(table, arg.text))
          return hoverResult(a, arg.span, valueMarkdown(*entry), context);
        if (!isStagePragma(pragma->name))
          return nullptr;
        break;
      }
    }
    for (size_t u : units) {
      for (const HlslInclude &include : a.units[u].scan.includes) {
        if (inside(include.pathSpan, offset) && u == unit) {
          auto project = UnityProject::forFile(a.path, context.editorOverride);
          auto resolved = project->resolveInclude(include.path, a.path.parent_path());
          return hoverResult(a, include.pathSpan, resolved ? "`" + displayPath(*resolved) + "`" : "Include file not found.", context);
        }
      }
    }
    Span word = wordAt(a.text, offset);
    if (word.empty())
      return nullptr;
    std::string name = spanText(a, word);
    if (const HlslDecl *decl = findLocalDecl(a, units, name))
      return hoverResult(a, word, declMarkdown(*decl), context);
    if (a.kind == DocumentKind::ShaderLab) {
      if (const MaterialProperty *property = a.shader.findProperty(name)) {
        return hoverResult(a, word, propertyMarkdown(a, *property), context);
      }
    }
    if (hlsl::isIntrinsic(name))
      return hoverResult(a, word, "```hlsl\n" + name + "\n```\n\nHLSL intrinsic function", context);
    if (hlsl::isKeywordOrType(name))
      return nullptr;
    if (auto external = findExternalDecl(includedFiles(a, units, context), name)) {
      return hoverResult(a, word, declMarkdown(*external->decl, displayPath(external->file->path)), context);
    }
    return nullptr;
  }
  json symbol(const Analysis &a, std::string name, std::string detail, int kind, Span range, Span selection, json children, const FeatureContext &context) {
    if (range.end < selection.end || range.begin > selection.begin)
      range = selection;
    if (name.empty())
      name = "(unnamed)";
    json result{{"name", std::move(name)},
      {"kind", kind},
      {"range", toRange(a.text, a.lines, range, context.encoding)},
      {"selectionRange", toRange(a.text, a.lines, selection, context.encoding)}};
    if (!detail.empty())
      result["detail"] = std::move(detail);
    if (!children.empty())
      result["children"] = std::move(children);
    return result;
  }
  int symbolKindFor(DeclKind kind) {
    switch (kind) {
    case DeclKind::Function:
      return kSymFunction;
    case DeclKind::Struct:
      return kSymStruct;
    case DeclKind::CBuffer:
      return kSymNamespace;
    case DeclKind::Variable:
      return kSymVariable;
    case DeclKind::Field:
      return kSymField;
    case DeclKind::Macro:
      return kSymConstant;
    }
    return kSymVariable;
  }
  json declSymbols(const Analysis &a, const HlslScan &scan, const FeatureContext &context) {
    json children = json::array();
    for (const HlslDecl &decl : scan.decls) {
      if (decl.kind == DeclKind::Field || decl.kind == DeclKind::Macro)
        continue;
      children.push_back(symbol(a, decl.name, decl.detail, symbolKindFor(decl.kind), decl.nameSpan, decl.nameSpan, json::array(), context));
    }
    return children;
  }
} // namespace
json toRange(std::string_view text, const LineIndex &lines, Span span, Encoding encoding) {
  Position start = lines.toPosition(text, span.begin, encoding);
  Position end = lines.toPosition(text, std::max(span.end, span.begin), encoding);
  return {{"start", {{"line", start.line}, {"character", start.character}}},
    {"end", {{"line", end.line}, {"character", end.character}}}};
}
json completion(const Analysis &a, size_t offset, const FeatureContext &context) {
  if (auto unit = a.unitAt(offset)) {
    bool glsl = a.kind == DocumentKind::ShaderLab && a.shader.blocks[a.units[*unit].block].kind == BlockKind::GlslProgram;
    if (!glsl)
      return hlslCompletion(a, *unit, offset, context);
    return {{"isIncomplete", false}, {"items", json::array()}};
  }
  if (a.kind == DocumentKind::ShaderLab)
    return shaderLabCompletion(a, offset, context);
  return {{"isIncomplete", false}, {"items", json::array()}};
}
json hover(const Analysis &a, size_t offset, const FeatureContext &context) {
  if (auto unit = a.unitAt(offset))
    return hlslHover(a, *unit, offset, context);
  if (a.kind == DocumentKind::ShaderLab)
    return shaderLabHover(a, offset, context);
  return nullptr;
}
json definition(const Analysis &a, size_t offset, const FeatureContext &context) {
  auto local = [&](Span span) { return location(a.path, a.text, a.lines, span, context.encoding); };
  if (auto unit = a.unitAt(offset)) {
    std::vector<size_t> units = a.visibleUnits(*unit);
    auto project = UnityProject::forFile(a.path, context.editorOverride);
    for (const HlslInclude &include : a.units[*unit].scan.includes) {
      if (inside(include.pathSpan, offset)) {
        auto resolved = project->resolveInclude(include.path, a.path.parent_path());
        if (!resolved)
          return nullptr;
        return json{{"uri", pathToUri(*resolved)},
          {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 0}}}}}};
      }
    }
    Span word = wordAt(a.text, offset);
    if (word.empty())
      return nullptr;
    std::string name = spanText(a, word);
    if (const HlslDecl *decl = findLocalDecl(a, units, name))
      return local(decl->nameSpan);
    if (a.kind == DocumentKind::ShaderLab) {
      if (const MaterialProperty *property = a.shader.findProperty(name))
        return local(property->nameSpan);
    }
    if (hlsl::isKeywordOrType(name))
      return nullptr;
    if (auto external = findExternalDecl(includedFiles(a, units, context), name)) {
      const CachedFile &file = *external->file;
      return location(file.path, file.text, file.lines, external->decl->nameSpan, context.encoding);
    }
    return nullptr;
  }
  if (a.kind != DocumentKind::ShaderLab)
    return nullptr;
  for (auto [command, stencilField] : allCommands(a.shader)) {
    for (const CommandArg &arg : command->args) {
      if (arg.propertyRef && inside(arg.span, offset)) {
        if (const MaterialProperty *property = a.shader.findProperty(arg.text))
          return local(property->nameSpan);
        return nullptr;
      }
    }
  }
  for (const MaterialProperty &property : a.shader.properties) {
    if (!inside(property.nameSpan, offset))
      continue;
    // From a property to the HLSL variable that receives it.
    for (const HlslUnit &unit : a.units) {
      for (const HlslDecl &decl : unit.scan.decls) {
        if (decl.name == property.name && decl.kind == DeclKind::Variable)
          return local(decl.nameSpan);
      }
    }
  }
  return nullptr;
}
json documentSymbols(const Analysis &a, const FeatureContext &context) {
  json result = json::array();
  if (a.kind != DocumentKind::ShaderLab)
    return declSymbols(a, a.units[0].scan, context);
  const ShaderFile &shader = a.shader;
  auto blockSymbols = [&](int subShader, int pass) {
    json list = json::array();
    for (size_t i = 0; i < a.units.size(); ++i) {
      const CodeBlock &block = shader.blocks[a.units[i].block];
      if (block.subShader != subShader || block.pass != pass)
        continue;
      Span range{block.keyword.begin, block.endKeyword.empty() ? block.content.end : block.endKeyword.end};
      list.push_back(symbol(a, blockKeyword(block.kind), "", kSymPackage, range, block.keyword, declSymbols(a, a.units[i].scan, context), context));
    }
    return list;
  };
  if (!shader.hasShader)
    return blockSymbols(-1, -1);
  json children = json::array();
  if (!shader.propertiesKeyword.empty()) {
    json properties = json::array();
    for (const MaterialProperty &property : shader.properties) {
      properties.push_back(symbol(a, property.name, property.displayName + " (" + property.type + ")", kSymProperty, property.span, property.nameSpan, json::array(), context));
    }
    Span range = shader.propertiesKeyword;
    for (const Scope &scope : shader.scopes) {
      if (scope.kind == ScopeKind::Properties && scope.span.begin >= shader.propertiesKeyword.end) {
        range.end = scope.span.end;
        break;
      }
    }
    children.push_back(symbol(a, "Properties", "", kSymNamespace, range, shader.propertiesKeyword, properties, context));
  }
  for (json &block : blockSymbols(-1, -1))
    children.push_back(std::move(block));
  for (size_t s = 0; s < shader.subShaders.size(); ++s) {
    const SubShader &subShader = shader.subShaders[s];
    json subChildren = blockSymbols(static_cast<int>(s), -1);
    for (size_t p = 0; p < subShader.passes.size(); ++p) {
      const Pass &pass = subShader.passes[p];
      std::string lightMode;
      for (const Tag &tag : pass.tags) {
        if (iequals(tag.key, "LightMode"))
          lightMode = tag.value;
      }
      std::string name = pass.name.empty() ? "Pass" : "Pass \"" + pass.name + "\"";
      subChildren.push_back(symbol(a, name, lightMode, kSymObject, pass.span, pass.keyword, blockSymbols(static_cast<int>(s), static_cast<int>(p)), context));
    }
    std::string pipeline;
    for (const Tag &tag : subShader.tags) {
      if (iequals(tag.key, "RenderPipeline"))
        pipeline = tag.value;
    }
    children.push_back(symbol(a, "SubShader", pipeline, kSymModule, subShader.span, subShader.keyword, subChildren, context));
  }
  result.push_back(symbol(a, shader.name.empty() ? "Shader" : shader.name, "Shader", kSymClass, shader.span, shader.nameSpan.empty() ? shader.keyword : shader.nameSpan, children, context));
  return result;
}
} // namespace sls
