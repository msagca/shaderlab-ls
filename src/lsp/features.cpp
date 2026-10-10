#include "lsp/features.h"
#include <algorithm>
#include <cctype>
#include <deque>
#include <set>
#include <tuple>
#include <unordered_map>
#include "analysis/conditionals.h"
#include "common/util.h"
#include "hlsl/builtins.h"
#include "hlsl/lexer.h"
#include "hlsl/scopes.h"
#include "shaderlab/lexer.h"
#include "shaderlab/reference.h"
#include "unity/project.h"
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace sls {
namespace {
  // LSP CompletionItemKind
  enum ItemKind {
    kItemMethod = 2,
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
  std::string typeMarkdown(const hlsl::TypeDoc &type) {
    return "```hlsl\n" + type.detail + "\n```\n\n" + type.doc;
  }
  std::string spanText(const Analysis &a, Span span) {
    return a.text.substr(span.begin, span.end - span.begin);
  }
  // The nearest character before `begin` on the same line that is not a space or tab; '\n' at the start of a line.
  char charBefore(std::string_view text, size_t begin) {
    while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t'))
      --begin;
    return begin > 0 && text[begin - 1] != '\r' ? text[begin - 1] : '\n';
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
    std::vector<std::shared_ptr<const CachedFile>> files = sls::includedFiles(a, units, context.editorOverride);
    files.insert(files.end(), context.outer.begin(), context.outer.end());
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
  // The #if conditions around a use in `unit`, which pick among the declarations of a name in different branches.
  std::vector<Guard> guardsAround(const Analysis &a, size_t unit, size_t offset) {
    return guardsAt(a.text, a.units[unit].range, offset);
  }
  // A declaration a use can see: one whose #if the use's own conditions don't rule out, and one they guarantee before
  // one they leave unsettled; then one in code the variant compiles before one an #if leaves out; then the fewer
  // conditions left unsettled the better; then by kind.
  using DeclRank = std::tuple<bool, bool, bool, int, int>;
  DeclRank declRank(const HlslDecl &decl, std::string_view text, Span range, const std::vector<Guard> &use) {
    GuardFit fit = guardFit(guardsAt(text, range, decl.nameSpan.begin), use);
    return {fit.excluded, fit.unsettled > 0, !decl.active, fit.unsettled, declPriority(decl.kind)};
  }
  const HlslDecl *bestDecl(const Analysis &a, size_t unit, const std::vector<const HlslDecl *> &candidates, const std::vector<Guard> &use) {
    const HlslDecl *best = nullptr;
    DeclRank bestRank;
    for (const HlslDecl *decl : candidates) {
      DeclRank rank = declRank(*decl, a.text, a.units[unit].range, use);
      if (!best || rank < bestRank)
        best = decl, bestRank = rank;
    }
    return best;
  }
  const HlslDecl *findLocalDecl(const Analysis &a, const std::vector<size_t> &units, std::string_view name, const std::vector<Guard> &use) {
    const HlslDecl *best = nullptr;
    DeclRank bestRank;
    for (size_t unit : units) {
      for (const HlslDecl &decl : a.units[unit].scan.decls) {
        if (decl.name != name)
          continue;
        DeclRank rank = declRank(decl, a.text, a.units[unit].range, use);
        if (!best || rank < bestRank)
          best = &decl, bestRank = rank;
      }
    }
    return best;
  }
  std::optional<ExternalDecl> findExternalDecl(const std::vector<std::shared_ptr<const CachedFile>> &files, std::string_view name, const std::vector<Guard> &use = {}) {
    std::optional<ExternalDecl> best;
    DeclRank bestRank;
    for (const auto &file : files) {
      for (const HlslDecl &decl : file->scan.decls) {
        if (decl.name != name || decl.kind == DeclKind::Field)
          continue;
        DeclRank rank = declRank(decl, file->text, {0, file->text.size()}, use);
        if (!best || rank < bestRank)
          best = ExternalDecl{file, &decl}, bestRank = rank;
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
    if (!decl.doc.empty())
      text += "\n\n" + decl.doc;
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
  // ---- expression types, for members after '.' --------------------------------
  // Enough of HLSL's types to say what follows a '.': the declared type of a parameter, local, global or field,
  // read from its declaration, and what indexing, a swizzle or a method call makes of it. Lexical like the rest:
  // a type hidden behind a macro other than Unity's texture macros is not seen through.
  struct CodeToken {
    enum Kind { Ident,
      Number,
      String,
      Punct } kind;
    Span span;
  };
  // The tokens of text[range], without comments and directive lines.
  std::vector<CodeToken> codeTokens(std::string_view text, Span range) {
    std::vector<CodeToken> result;
    for (const HlslToken &token : lexHlsl(text, range)) {
      switch (token.kind) {
      case HlslTok::Ident:
        result.push_back({CodeToken::Ident, token.span});
        break;
      case HlslTok::Number:
        result.push_back({CodeToken::Number, token.span});
        break;
      case HlslTok::String:
        result.push_back({CodeToken::String, token.span});
        break;
      case HlslTok::Punct:
        result.push_back({CodeToken::Punct, token.span});
        break;
      default:
        break;
      }
    }
    return result;
  }
  struct TypeRef {
    std::string name;
    std::string element; // the template argument: Foo for StructuredBuffer<Foo>
  };
  // Unity's texture declaration macros, TEXTURE2D(_BaseMap) and the like, as the type they declare.
  std::string canonicalType(std::string_view name) {
    bool upper = !name.empty() && std::none_of(name.begin(), name.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); });
    if (!upper)
      return std::string(name);
    auto has = [&](std::string_view part) { return name.find(part) != std::string_view::npos; };
    std::string prefix = has("RW_") ? "RW" : "";
    bool array = has("ARRAY");
    if (has("TEXTURECUBE") || has("TEXCUBE"))
      return prefix + (array ? "TextureCubeArray" : "TextureCube");
    if (has("TEXTURE3D") || has("TEX3D"))
      return prefix + "Texture3D";
    if (has("TEXTURE2D") || has("TEX2D"))
      return prefix + (array ? "Texture2DArray" : "Texture2D");
    return std::string(name);
  }
  class TypeReader {
  public:
    TypeReader(std::string_view text, const std::vector<CodeToken> &tokens)
      : text_(text), tokens_(tokens) {}
    std::string_view str(size_t i) const {
      return text_.substr(tokens_[i].span.begin, tokens_[i].span.end - tokens_[i].span.begin);
    }
    bool punct(size_t i, char c) const {
      return i < tokens_.size() && tokens_[i].kind == CodeToken::Punct && text_[tokens_[i].span.begin] == c;
    }
    // The type declared before the name at tokens[k]: `float4 color`, `StructuredBuffer<Foo> buffer`, the `b` of
    // `float a, b`, or Unity's `TEXTURE2D(_BaseMap)`.
    std::optional<TypeRef> before(size_t k) const {
      if (k == 0)
        return std::nullopt;
      size_t p = k - 1;
      if (punct(p, ',')) {
        // Back to the start of the declaration, past the names declared before this one.
        int depth = 0;
        while (p > 0) {
          --p;
          if (punct(p, ')') || punct(p, ']') || punct(p, '>'))
            ++depth;
          if (punct(p, '(') || punct(p, '[') || punct(p, '<')) {
            if (depth == 0)
              break;
            --depth;
          }
          if (depth == 0 && (punct(p, ';') || punct(p, '{') || punct(p, '}')))
            break;
        }
        size_t first = tokens_[p].kind == CodeToken::Ident && p == 0 ? 0 : p + 1;
        while (first < k && tokens_[first].kind == CodeToken::Ident && hlsl::findKeyword(str(first)))
          ++first;
        return first < k && tokens_[first].kind == CodeToken::Ident ? at(first) : std::nullopt;
      }
      if (punct(p, '>')) {
        int depth = 0;
        for (size_t q = p + 1; q-- > 0;) {
          if (punct(q, '>'))
            ++depth;
          if (punct(q, '<') && --depth == 0)
            return q > 0 && tokens_[q - 1].kind == CodeToken::Ident ? at(q - 1) : std::nullopt;
        }
        return std::nullopt;
      }
      if (tokens_[p].kind == CodeToken::Ident)
        return hlsl::findKeyword(str(p)) ? std::nullopt : at(p);
      if (punct(p, '(') && p > 0 && tokens_[p - 1].kind == CodeToken::Ident && punct(k + 1, ')')) {
        std::string macro = canonicalType(str(p - 1));
        if (macro != str(p - 1))
          return TypeRef{macro, {}};
      }
      return std::nullopt;
    }
    // The type named at tokens[i], with its template argument if one follows.
    std::optional<TypeRef> at(size_t i) const {
      TypeRef type{canonicalType(str(i)), {}};
      if (punct(i + 1, '<')) {
        size_t j = i + 2;
        while (j < tokens_.size() && !punct(j, ',') && !punct(j, '>'))
          ++j;
        if (j > i + 2)
          type.element = std::string(text_.substr(tokens_[i + 2].span.begin, tokens_[j - 1].span.end - tokens_[i + 2].span.begin));
      }
      return type;
    }
  private:
    std::string_view text_;
    const std::vector<CodeToken> &tokens_;
  };
  // The type a declaration's text gives `name`: "float4 _Color", "TEXTURE2D(_BaseMap)", "Varyings vert(...)".
  std::optional<TypeRef> typeInDetail(std::string_view detail, std::string_view name) {
    std::vector<CodeToken> tokens = codeTokens(detail, {0, detail.size()});
    TypeReader reader(detail, tokens);
    for (size_t k = 0; k < tokens.size(); ++k) {
      if (tokens[k].kind == CodeToken::Ident && reader.str(k) == name)
        return reader.before(k);
    }
    return std::nullopt;
  }
  // A '.' and what comes before it: `input.uv`, `_Lights[i].color`, `GetData().position`.
  struct Segment {
    std::string name;
    int indexes = 0; // [i] after it
    bool call = false; // (...) after it
  };
  std::optional<std::vector<Segment>> chainBefore(std::string_view text, size_t dot, size_t floor) {
    std::vector<Segment> chain;
    auto skipSpaces = [&](size_t p) {
      while (p > floor && (text[p - 1] == ' ' || text[p - 1] == '\t'))
        --p;
      return p;
    };
    // The '(' or '[' that the ')' or ']' just before `p` closes.
    auto opening = [&](size_t p, char open, char close) -> std::optional<size_t> {
      int depth = 0;
      while (p > floor) {
        --p;
        if (text[p] == close)
          ++depth;
        if (text[p] == open && --depth == 0)
          return p;
        if (text[p] == ';' || text[p] == '{' || text[p] == '}')
          return std::nullopt;
      }
      return std::nullopt;
    };
    size_t pos = dot;
    while (true) {
      Segment segment;
      size_t p = skipSpaces(pos);
      while (p > floor && text[p - 1] == ']') {
        auto open = opening(p, '[', ']');
        if (!open)
          return std::nullopt;
        ++segment.indexes;
        p = skipSpaces(*open);
      }
      if (p > floor && text[p - 1] == ')') {
        auto open = opening(p, '(', ')');
        if (!open)
          return std::nullopt;
        segment.call = true;
        p = skipSpaces(*open);
      }
      size_t end = p;
      while (p > floor && isIdentChar(text[p - 1]))
        --p;
      if (p == end || std::isdigit(static_cast<unsigned char>(text[p])))
        return std::nullopt;
      segment.name = std::string(text.substr(p, end - p));
      chain.insert(chain.begin(), std::move(segment));
      size_t q = skipSpaces(p);
      if (q <= floor || text[q - 1] != '.')
        return chain;
      pos = q - 1;
    }
  }
  struct FoundDecl {
    const HlslDecl *decl;
    std::shared_ptr<const CachedFile> file; // null for a declaration in the document itself
    std::string where() const {
      return file ? displayPath(file->path) : std::string();
    }
  };
  class TypeResolver {
  public:
    TypeResolver(const Analysis &a, size_t unit, const FeatureContext &context)
      : a_(a), unit_(unit), units_(a.visibleUnits(unit)), files_(includedFiles(a, units_, context)) {}
    // The declaration of `name` among `kinds`, in this document before its includes.
    std::optional<FoundDecl> find(std::string_view name, std::initializer_list<DeclKind> kinds, std::string_view container = {}) const {
      auto matches = [&](const HlslDecl &decl) {
        return decl.name == name && std::find(kinds.begin(), kinds.end(), decl.kind) != kinds.end() && (container.empty() || decl.container == container);
      };
      // Code the variant compiles first: a field an #if swaps for another is the one compiled.
      for (bool active : {true, false}) {
        for (size_t u : units_) {
          for (const HlslDecl &decl : a_.units[u].scan.decls) {
            if (decl.active == active && matches(decl))
              return FoundDecl{&decl, nullptr};
          }
        }
      }
      for (const auto &file : files_) {
        for (const HlslDecl &decl : file->scan.decls) {
          if (matches(decl))
            return FoundDecl{&decl, file};
        }
      }
      return std::nullopt;
    }
    // The fields of a struct, from wherever it is declared.
    std::vector<FoundDecl> fields(std::string_view structName) const {
      std::vector<FoundDecl> result;
      auto add = [&](const std::vector<HlslDecl> &decls, const std::shared_ptr<const CachedFile> &file) {
        for (const HlslDecl &decl : decls) {
          if (decl.kind == DeclKind::Field && decl.container == structName && decl.active)
            result.push_back({&decl, file});
        }
      };
      for (size_t u : units_)
        add(a_.units[u].scan.decls, nullptr);
      if (result.empty()) {
        for (const auto &file : files_) {
          add(file->scan.decls, file);
          if (!result.empty())
            break;
        }
      }
      return result;
    }
    // The type of the expression that ends just before the '.' at `dot`.
    std::optional<TypeRef> typeBefore(size_t dot) const {
      auto chain = chainBefore(a_.text, dot, a_.units[unit_].range.begin);
      if (!chain)
        return std::nullopt;
      std::optional<TypeRef> type = baseType(chain->front(), dot);
      for (size_t i = 0; type && i < chain->size(); ++i) {
        const Segment &segment = (*chain)[i];
        if (i > 0)
          type = member(*type, segment);
        for (int k = 0; type && k < segment.indexes; ++k)
          type = element(*type);
      }
      return type;
    }
    std::optional<TypeRef> member(const TypeRef &type, const Segment &segment) const {
      if (segment.call) {
        if (hlsl::methodsOf(type.name).empty())
          return std::nullopt;
        if (segment.name.starts_with("SampleCmp") || segment.name.starts_with("CalculateLevelOfDetail"))
          return TypeRef{"float", {}};
        if (segment.name.starts_with("Gather"))
          return TypeRef{"float4", {}};
        return TypeRef{type.element.empty() ? "float4" : type.element, {}};
      }
      if (auto shape = hlsl::numericShape(type.name)) {
        if (shape->columns > 0)
          return segment.name.starts_with("_") ? std::optional(TypeRef{shape->scalar, {}}) : std::nullopt;
        size_t size = segment.name.size();
        if (size > 4)
          return std::nullopt;
        return TypeRef{size == 1 ? shape->scalar : shape->scalar + std::to_string(size), {}};
      }
      if (auto member = find(segment.name, {DeclKind::Field, DeclKind::Function}, type.name))
        return typeInDetail(member->decl->detail, segment.name);
      return std::nullopt;
    }
  private:
    std::optional<TypeRef> baseType(const Segment &base, size_t dot) const {
      if (base.call) {
        if (hlsl::numericShape(base.name))
          return TypeRef{base.name, {}}; // a constructor: float3(0, 0, 1)
        if (auto function = find(base.name, {DeclKind::Function}))
          return typeInDetail(function->decl->detail, base.name);
        return std::nullopt;
      }
      if (auto local = localType(base.name, dot))
        return local;
      if (auto global = find(base.name, {DeclKind::Variable}))
        return typeInDetail(global->decl->detail, base.name);
      return std::nullopt;
    }
    // A parameter or local declared before `offset` in the top-level declaration it is in.
    std::optional<TypeRef> localType(std::string_view name, size_t offset) const {
      std::vector<CodeToken> tokens = codeTokens(a_.text, {a_.units[unit_].range.begin, offset});
      TypeReader reader(a_.text, tokens);
      size_t start = 0;
      int depth = 0;
      for (size_t i = 0; i < tokens.size(); ++i) {
        if (reader.punct(i, '{'))
          ++depth;
        if (reader.punct(i, '}') && depth > 0 && --depth == 0)
          start = i + 1;
        if (reader.punct(i, ';') && depth == 0)
          start = i + 1;
      }
      std::optional<TypeRef> found;
      for (size_t k = start; k < tokens.size(); ++k) {
        if (tokens[k].kind != CodeToken::Ident || reader.str(k) != name)
          continue;
        bool declarator = k + 1 < tokens.size() && (reader.punct(k + 1, ';') || reader.punct(k + 1, ',') || reader.punct(k + 1, '=') ||
                                                      reader.punct(k + 1, ')') || reader.punct(k + 1, ':') || reader.punct(k + 1, '['));
        if (!declarator)
          continue;
        if (auto type = reader.before(k))
          found = type;
      }
      return found;
    }
    std::optional<TypeRef> element(const TypeRef &type) const {
      if (!type.element.empty())
        return TypeRef{type.element, {}};
      if (auto shape = hlsl::numericShape(type.name)) {
        if (shape->columns > 0)
          return TypeRef{shape->scalar + std::to_string(shape->columns), {}};
        return TypeRef{shape->scalar, {}};
      }
      if (!hlsl::methodsOf(type.name).empty())
        return TypeRef{"float4", {}}; // Texture2D without <T> holds float4
      return type; // an array
    }
    const Analysis &a_;
    size_t unit_;
    std::vector<size_t> units_;
    std::vector<std::shared_ptr<const CachedFile>> files_;
  };
  void addMembers(Items &items, const TypeResolver &resolver, const TypeRef &type) {
    if (auto shape = hlsl::numericShape(type.name)) {
      if (shape->columns > 0) {
        for (int r = 0; r < shape->rows; ++r) {
          for (int c = 0; c < shape->columns; ++c)
            items.add("_m" + std::to_string(r) + std::to_string(c), kItemField, shape->scalar, "Row " + std::to_string(r) + ", column " + std::to_string(c) + ".");
        }
        return;
      }
      // A scalar has an x too: 1.0.xxx is a float3.
      int size = shape->rows;
      for (std::string_view set : {"xyzw", "rgba"}) {
        for (int i = 0; i < size; ++i)
          items.add(std::string(1, set[i]), kItemField, shape->scalar);
        for (int length = 2; length <= size; ++length)
          items.add(std::string(set.substr(0, length)), kItemField, shape->scalar + std::to_string(length));
      }
      return;
    }
    for (const ref::Entry *method : hlsl::methodsOf(type.name))
      items.add(std::string(method->name), kItemMethod, {}, entryMarkdown(*method, "hlsl"));
    for (const FoundDecl &field : resolver.fields(type.name))
      items.add(field.decl->name, kItemField, field.decl->detail, declMarkdown(*field.decl, field.where()));
  }
  // The '.' before `begin`, if there is one with only spaces between.
  std::optional<size_t> dotBefore(std::string_view text, size_t begin, size_t floor) {
    while (begin > floor && (text[begin - 1] == ' ' || text[begin - 1] == '\t'))
      --begin;
    return begin > floor && text[begin - 1] == '.' ? std::optional(begin - 1) : std::nullopt;
  }
  // The field or method a member access names, `uv` in `input.uv`, found through the type of what is before the '.'.
  std::optional<FoundDecl> memberDecl(const Analysis &a, size_t unit, Span word, const FeatureContext &context) {
    auto dot = dotBefore(a.text, word.begin, a.units[unit].range.begin);
    if (!dot)
      return std::nullopt;
    TypeResolver resolver(a, unit, context);
    auto type = resolver.typeBefore(*dot);
    if (!type)
      return std::nullopt;
    return resolver.find(a.text.substr(word.begin, word.end - word.begin), {DeclKind::Field, DeclKind::Function}, type->name);
  }
  // Where the parameter or local `word` names is declared, by block scope, if it names one.
  std::optional<size_t> localDecl(const Analysis &a, size_t unit, Span word) {
    std::unordered_map<size_t, size_t> bindings = localBindings(a.text, a.units[unit].range);
    auto it = bindings.find(word.begin);
    return it == bindings.end() ? std::nullopt : std::optional(it->second);
  }
  // A parameter or local as it is declared, `out float3 normal` or `int i`: from the '(', ',' or start of the
  // statement before its name up to the name. A ',' inside <> such as vector<float, 3>'s is part of the type.
  std::string localMarkdown(const Analysis &a, size_t unit, size_t declaration) {
    size_t floor = a.units[unit].range.begin;
    size_t begin = declaration;
    int angle = 0;
    for (; begin > floor; --begin) {
      char c = a.text[begin - 1];
      if (c == '>')
        ++angle;
      else if (c == '<')
        --angle;
      else if (angle <= 0 && (c == '(' || c == ',' || c == ';' || c == '{' || c == '}'))
        break;
    }
    size_t end = wordAt(a.text, declaration).end;
    // A name a parameter macro declares, tex in TEXTURE2D_PARAM(tex, samp): the macro is its declaration.
    bool bare = trim(std::string_view(a.text).substr(begin, end - begin)) == std::string_view(a.text).substr(declaration, end - declaration);
    if (bare && begin > floor && (a.text[begin - 1] == '(' || a.text[begin - 1] == ',')) {
      size_t open = begin - 1;
      while (open > floor && a.text[open] != '(' && a.text[open] != ')' && a.text[open] != ';' && a.text[open] != '{' && a.text[open] != '}')
        --open;
      Span macro = wordAt(a.text, open);
      size_t close = a.text.find(')', end);
      if (a.text[open] == '(' && !macro.empty() && close != std::string::npos &&
          std::none_of(a.text.begin() + macro.begin, a.text.begin() + macro.end, [](char c) { return std::islower(static_cast<unsigned char>(c)); }))
        begin = macro.begin, end = close + 1;
    }
    std::string text;
    bool space = false;
    for (char c : a.text.substr(begin, end - begin)) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        space = !text.empty();
        continue;
      }
      if (space)
        text.push_back(' ');
      space = false;
      text.push_back(c);
    }
    return "```hlsl\n" + text + "\n```";
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
    // After a '.', only the members of what is before it; nothing at all when its type can't be told, rather than
    // every name in scope.
    size_t wordStart = offset;
    while (wordStart > a.units[unit].range.begin && isIdentChar(a.text[wordStart - 1]))
      --wordStart;
    if (wordStart > a.units[unit].range.begin && a.text[wordStart - 1] == '.') {
      TypeResolver resolver(a, unit, context);
      if (auto type = resolver.typeBefore(wordStart - 1))
        addMembers(items, resolver, *type);
      return items.result();
    }
    // What the variant compiles first, so that of two declarations of a name the one shown is the one compiled.
    for (bool active : {true, false}) {
      for (size_t u : units) {
        for (const HlslDecl &decl : a.units[u].scan.decls) {
          if (decl.active == active)
            addDecl(items, decl);
        }
      }
    }
    if (a.kind == DocumentKind::ShaderLab) {
      for (const MaterialProperty &property : a.shader.properties) {
        items.add(property.name, kItemProperty, "Material property (" + property.type + ")", propertyMarkdown(a, property));
      }
    }
    for (const ref::Entry &keyword : hlsl::keywords())
      items.add(std::string(keyword.name), kItemKeyword, {}, entryMarkdown(keyword, "hlsl"));
    for (const std::string &type : hlsl::types()) {
      auto doc = hlsl::describeType(type);
      items.add(type, kItemClass, {}, doc ? typeMarkdown(*doc) : std::string());
    }
    // Everything goes in the documentation, for the client's side window; the list itself shows just the name.
    for (const ref::Entry &function : hlsl::intrinsics())
      items.add(std::string(function.name), kItemFunction, {}, entryMarkdown(function, "hlsl"));
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
      // A built-in texture named as a texture property's default: = "white" {}
      bool texture = false;
      for (std::string_view type : {"2D", "2DArray", "3D", "Cube", "CubeArray", "Any"})
        texture = texture || iequals(property.type, type);
      if (texture && inside(property.span, offset) && offset > property.typeSpan.end) {
        Span word = wordAt(a.text, offset);
        if (const ref::Entry *entry = ref::find(ref::textureDefaults(), spanText(a, word)))
          return hoverResult(a, word, valueMarkdown(*entry), context);
      }
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
    // Legacy fixed-function commands are skipped by the parser, so they are recognised where they stand: first on
    // their line or just inside a block, as in Fog { Mode Off }.
    char before = charBefore(a.text, word.begin);
    if (before == '\n' || before == '{') {
      if (const ref::Entry *entry = ref::find(ref::legacyCommands(), name))
        return hoverResult(a, word, entryMarkdown(*entry), context);
      if (const ref::Entry *entry = ref::find(ref::legacySubCommands(), name))
        return hoverResult(a, word, entryMarkdown(*entry), context);
    }
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
    // What comes before the word tells a directive (#if), an attribute ([unroll]) and a semantic (: SV_Target) apart
    // from an identifier of the same name.
    char before = charBefore(a.text, word.begin);
    if (before == '#') {
      if (const ref::Entry *entry = ref::find(ref::preprocessorDirectives(), name))
        return hoverResult(a, word, entryMarkdown(*entry, "hlsl"), context);
    }
    if (before == ':') {
      if (const ref::Entry *entry = hlsl::findSemantic(name))
        return hoverResult(a, word, entryMarkdown(*entry, "hlsl"), context);
    }
    if (before == '.') {
      if (auto field = memberDecl(a, unit, word, context))
        return hoverResult(a, word, declMarkdown(*field->decl, field->where()), context);
    }
    if (before != '.') {
      if (auto declaration = localDecl(a, unit, word))
        return hoverResult(a, word, localMarkdown(a, unit, *declaration), context);
    }
    std::vector<Guard> use = guardsAround(a, unit, word.begin);
    const HlslDecl *decl = findLocalDecl(a, units, name, use);
    if (before == '[' && !decl) {
      if (const ref::Entry *entry = hlsl::findAttribute(name))
        return hoverResult(a, word, entryMarkdown(*entry, "hlsl"), context);
    }
    if (decl)
      return hoverResult(a, word, declMarkdown(*decl), context);
    if (a.kind == DocumentKind::ShaderLab) {
      if (const MaterialProperty *property = a.shader.findProperty(name)) {
        return hoverResult(a, word, propertyMarkdown(a, *property), context);
      }
    }
    if (before == '.') {
      if (const ref::Entry *method = hlsl::findMethod(name))
        return hoverResult(a, word, entryMarkdown(*method, "hlsl"), context);
    }
    if (const ref::Entry *intrinsic = hlsl::findIntrinsic(name))
      return hoverResult(a, word, entryMarkdown(*intrinsic, "hlsl"), context);
    if (const ref::Entry *keyword = hlsl::findKeyword(name))
      return hoverResult(a, word, entryMarkdown(*keyword, "hlsl"), context);
    if (auto type = hlsl::describeType(name))
      return hoverResult(a, word, typeMarkdown(*type), context);
    if (auto external = findExternalDecl(includedFiles(a, units, context), name, use)) {
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
  // ---- references and rename ---------------------------------------------------
  // An identifier in code. `item` numbers the top-level declarations of a unit — a function with its parameters and
  // body, a statement at file scope, a directive line — and is the scope a name nothing declares is searched in.
  struct Identifier {
    Span span;
    bool member = false; // follows '.': a struct member, a method or a swizzle
    size_t item = 0;
    std::optional<size_t> local; // where the parameter or local it names is declared, by block scope
  };
  // Every identifier in text[range], outside comments and strings. Unlike the declaration scan this reads directive
  // lines too, so `#pragma vertex vert` and macro bodies count as uses; only #include paths are skipped.
  std::vector<Identifier> identifiersIn(std::string_view text, Span range) {
    std::vector<Identifier> result;
    size_t i = range.begin;
    size_t end = std::min(range.end, text.size());
    int depth = 0;
    size_t item = 0;
    bool lineStart = true;
    bool directive = false;
    char last = 0;
    while (i < end) {
      char c = text[i];
      if (c == '\n') {
        size_t back = i;
        while (back > range.begin && text[back - 1] == '\r')
          --back;
        if (directive && !(back > range.begin && text[back - 1] == '\\')) {
          directive = false;
          if (depth == 0)
            ++item;
        }
        lineStart = true;
        ++i;
        continue;
      }
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++i;
        continue;
      }
      if (c == '/' && i + 1 < end && text[i + 1] == '/') {
        while (i < end && text[i] != '\n')
          ++i;
        continue;
      }
      if (c == '/' && i + 1 < end && text[i + 1] == '*') {
        size_t close = text.find("*/", i + 2);
        i = close == std::string_view::npos || close + 2 > end ? end : close + 2;
        continue;
      }
      if (c == '#' && lineStart) {
        lineStart = false;
        directive = true;
        if (depth == 0)
          ++item;
        size_t word = i + 1;
        while (word < end && (text[word] == ' ' || text[word] == '\t'))
          ++word;
        if (text.substr(word, 7) == "include") {
          while (i < end && text[i] != '\n')
            ++i;
          continue;
        }
        last = c;
        ++i;
        continue;
      }
      lineStart = false;
      if (c == '"' || c == '\'') {
        ++i;
        while (i < end && text[i] != c && text[i] != '\n')
          i += text[i] == '\\' ? 2 : 1;
        i = std::min(i + 1, end);
        last = '"';
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(c))) {
        while (i < end && (isIdentChar(text[i]) || text[i] == '.'))
          ++i;
        last = '0';
        continue;
      }
      if (isIdentStart(c)) {
        size_t begin = i;
        while (i < end && isIdentChar(text[i]))
          ++i;
        result.push_back({{begin, i}, last == '.', item});
        last = 'a';
        continue;
      }
      if (!directive) {
        if (c == '{') {
          ++depth;
        } else if (c == '}') {
          if (depth > 0 && --depth == 0)
            ++item;
        } else if (c == ';' && depth == 0) {
          ++item;
        }
      }
      last = c;
      ++i;
    }
    return result;
  }
  // The identifiers of one unit, each with the parameter or local it names, if any.
  std::vector<Identifier> unitIdentifiers(const Analysis &a, size_t unit) {
    std::vector<Identifier> ids = identifiersIn(a.text, a.units[unit].range);
    std::unordered_map<size_t, size_t> bindings = localBindings(a.text, a.units[unit].range);
    for (Identifier &id : ids) {
      if (auto it = bindings.find(id.span.begin); it != bindings.end() && !id.member)
        id.local = it->second;
    }
    return ids;
  }
  // Unity binds a texture property's scale/offset, texel size, HDR decode values and sampler by name, so renaming
  // the texture renames these with it.
  bool isTextureProperty(const MaterialProperty &property) {
    static const char *const kTypes[] = {"2D", "3D", "Cube", "2DArray", "CubeArray", "Any"};
    return std::any_of(std::begin(kTypes), std::end(kTypes), [&](const char *type) { return iequals(property.type, type); });
  }
  std::vector<std::pair<std::string, std::string>> textureCompanions(const std::string &from, const std::string &to) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const char *suffix : {"_ST", "_TexelSize", "_HDR"})
      result.emplace_back(from + suffix, to + suffix);
    result.emplace_back("sampler" + from, "sampler" + to);
    return result;
  }
  // The symbol at an offset and every place this document names it.
  struct SymbolUses {
    enum class Kind {
      Property,
      Global, // declared in this document
      External, // declared in an included file
      Member,
      Local,
    };
    Kind kind = Kind::Global;
    std::string name;
    Span at; // the occurrence the request was made on
    std::optional<Span> declaration;
    std::vector<Span> spans; // in document order, the declaration among them
    std::vector<size_t> units; // the code the symbol is visible in
    std::optional<size_t> item; // Local only: the declaration it is local to
    std::string refusal; // why it can't be renamed; empty when it can
    // The include file declaring it, which every file including it may name it too: an External symbol's, a Global's
    // in an include file, or the one that declares the shader variable a Property binds to.
    std::optional<fs::path> declFile;
  };
  bool isBuiltinName(std::string_view name) {
    return hlsl::isKeywordOrType(name) || hlsl::findIntrinsic(name);
  }
  // The units that see code declared in `unit`: an include block is seen by itself, the other include blocks and
  // the programs of its kind; a program block or an HLSL document only by itself.
  std::vector<size_t> unitsSeeing(const Analysis &a, size_t unit) {
    std::vector<size_t> result;
    for (size_t i = 0; i < a.units.size(); ++i) {
      if (a.isGlsl(i))
        continue;
      std::vector<size_t> visible = a.visibleUnits(i);
      if (std::find(visible.begin(), visible.end(), unit) != visible.end())
        result.push_back(i);
    }
    return result;
  }
  // `[_Prop]` in ShaderLab, outside Properties, where attributes are written the same way. From the tokens rather
  // than the parsed commands, so the fixed-function commands the parser skips (`SetTexture [_MainTex]`) count too.
  std::vector<std::pair<std::string, Span>> propertyRefs(const Analysis &a) {
    std::vector<std::pair<std::string, Span>> result;
    if (a.kind != DocumentKind::ShaderLab)
      return result;
    std::vector<Span> properties;
    for (const Scope &scope : a.shader.scopes) {
      if (scope.kind == ScopeKind::Properties)
        properties.push_back(scope.span);
    }
    std::vector<Token> tokens = lexShaderLab(a.text);
    for (size_t k = 0; k + 2 < tokens.size(); ++k) {
      if (tokens[k].kind != Tok::LBracket || tokens[k + 1].kind != Tok::Ident || tokens[k + 2].kind != Tok::RBracket)
        continue;
      Span span = tokens[k + 1].span;
      if (std::none_of(properties.begin(), properties.end(), [&](Span scope) { return inside(scope, span.begin); }))
        result.emplace_back(std::string(tokens[k + 1].text), span);
    }
    return result;
  }
  void sortSpans(std::vector<Span> &spans) {
    std::sort(spans.begin(), spans.end(), [](Span x, Span y) { return x.begin < y.begin; });
    spans.erase(std::unique(spans.begin(), spans.end(), [](Span x, Span y) { return x.begin == y.begin; }), spans.end());
  }
  // A material property: its declaration, its `[_Prop]` references and the shader variables Unity binds it to, in
  // GLSL blocks as well as HLSL ones.
  SymbolUses propertyUses(const Analysis &a, const std::string &name, Span at) {
    SymbolUses uses{SymbolUses::Kind::Property, name, at};
    if (const MaterialProperty *property = a.shader.findProperty(name)) {
      uses.declaration = property->nameSpan;
      uses.spans.push_back(property->nameSpan);
    }
    for (const auto &[text, span] : propertyRefs(a)) {
      if (text == name)
        uses.spans.push_back(span);
    }
    for (size_t i = 0; i < a.units.size(); ++i) {
      uses.units.push_back(i);
      for (const Identifier &id : unitIdentifiers(a, i)) {
        if (!id.member && !id.local && spanText(a, id.span) == name)
          uses.spans.push_back(id.span);
      }
    }
    sortSpans(uses.spans);
    return uses;
  }
  SymbolUses propertyUses(const Analysis &a, const std::string &name, Span at, const FeatureContext &context) {
    SymbolUses uses = propertyUses(a, name, at);
    // The variable it binds to, when an include file declares it rather than the shader.
    std::vector<size_t> code;
    bool declaredHere = false;
    for (size_t i = 0; i < a.units.size(); ++i) {
      if (a.isGlsl(i))
        continue;
      code.push_back(i);
      for (const HlslDecl &decl : a.units[i].scan.decls)
        declaredHere = declaredHere || (decl.name == name && decl.kind == DeclKind::Variable);
    }
    if (!declaredHere) {
      if (auto external = findExternalDecl(includedFiles(a, code, context), name); external && external->decl->kind == DeclKind::Variable)
        uses.declFile = external->file->path;
    }
    return uses;
  }
  std::optional<SymbolUses> symbolUses(const Analysis &a, size_t offset, const FeatureContext &context) {
    auto unit = a.unitAt(offset);
    if (!unit) {
      if (a.kind != DocumentKind::ShaderLab)
        return std::nullopt;
      for (const MaterialProperty &property : a.shader.properties) {
        if (inside(property.nameSpan, offset))
          return propertyUses(a, property.name, property.nameSpan, context);
      }
      for (const auto &[name, span] : propertyRefs(a)) {
        if (inside(span, offset))
          return propertyUses(a, name, span, context);
      }
      return std::nullopt;
    }
    std::vector<Identifier> ids = unitIdentifiers(a, *unit);
    auto hit = std::find_if(ids.begin(), ids.end(), [&](const Identifier &id) { return inside(id.span, offset); });
    if (hit == ids.end())
      return std::nullopt;
    std::string name = spanText(a, hit->span);
    // A parameter or local: its declaration and the uses its block scope gives it, whatever else shares its name.
    if (hit->local && !a.isGlsl(*unit)) {
      SymbolUses uses{SymbolUses::Kind::Local, name, hit->span};
      uses.units = {*unit};
      uses.item = hit->item;
      uses.declaration = Span{*hit->local, *hit->local + name.size()};
      for (const Identifier &id : ids) {
        if (id.local == hit->local)
          uses.spans.push_back(id.span);
      }
      sortSpans(uses.spans);
      return uses;
    }
    if (!hit->member && a.kind == DocumentKind::ShaderLab && a.shader.findProperty(name))
      return propertyUses(a, name, hit->span, context);
    if (a.isGlsl(*unit) || (!hit->member && isBuiltinName(name)))
      return std::nullopt;
    // The declaration this unit sees, its own first.
    const HlslDecl *decl = nullptr;
    size_t declUnit = *unit;
    std::vector<size_t> visible = a.visibleUnits(*unit);
    std::stable_partition(visible.begin(), visible.end(), [&](size_t v) { return v == *unit; });
    std::vector<Guard> use = guardsAround(a, *unit, hit->span.begin);
    for (size_t v : visible) {
      std::vector<const HlslDecl *> candidates;
      for (const HlslDecl &candidate : a.units[v].scan.decls) {
        if (candidate.name == name)
          candidates.push_back(&candidate);
      }
      if (const HlslDecl *best = bestDecl(a, v, candidates, use)) {
        decl = best;
        declUnit = v;
        break;
      }
    }
    SymbolUses uses{SymbolUses::Kind::Global, name, hit->span};
    if (hit->member || (decl && decl->kind == DeclKind::Field)) {
      uses.kind = SymbolUses::Kind::Member;
      uses.units = decl ? unitsSeeing(a, declUnit) : visible;
      for (size_t v : uses.units) {
        for (const HlslDecl &field : a.units[v].scan.decls) {
          if (field.kind == DeclKind::Field && field.name == name)
            uses.spans.push_back(field.nameSpan);
        }
        for (const Identifier &id : identifiersIn(a.text, a.units[v].range)) {
          if (id.member && spanText(a, id.span) == name)
            uses.spans.push_back(id.span);
        }
      }
      uses.refusal = "Struct members are matched by name alone, not by their struct's type, so renaming " + name +
        " could also rename another struct's member or a swizzle.";
      sortSpans(uses.spans);
      return uses;
    }
    if (decl) {
      uses.declaration = decl->nameSpan;
      uses.units = unitsSeeing(a, declUnit);
      if (a.kind == DocumentKind::HlslInclude)
        uses.declFile = a.path;
    } else if (auto external = findExternalDecl(includedFiles(a, visible, context), name, use)) {
      uses.kind = SymbolUses::Kind::External;
      for (size_t v = 0; v < a.units.size(); ++v) {
        if (!a.isGlsl(v))
          uses.units.push_back(v);
      }
      uses.declFile = external->file->path;
      uses.refusal = name + " is declared in " + displayPath(external->file->path) + ", outside the workspace.";
    } else {
      // Nothing declares it at file scope: a parameter or a local, looked for in the declaration it is used in.
      uses.kind = SymbolUses::Kind::Local;
      uses.units = {*unit};
      uses.item = hit->item;
    }
    for (size_t v : uses.units) {
      for (const Identifier &id : v == *unit ? ids : unitIdentifiers(a, v)) {
        if (!id.member && !id.local && (!uses.item || id.item == *uses.item) && spanText(a, id.span) == name)
          uses.spans.push_back(id.span);
      }
    }
    sortSpans(uses.spans);
    return uses;
  }
  // Whether `name` is already used where `uses` would be renamed: the new name would collide with it or shadow it.
  bool nameInUse(const Analysis &a, const std::vector<size_t> &units, std::optional<size_t> item, bool property, std::string_view name) {
    if (property) {
      if (a.shader.findProperty(name))
        return true;
      for (const auto &[text, span] : propertyRefs(a)) {
        if (text == name)
          return true;
      }
    }
    for (size_t v : units) {
      for (const Identifier &id : identifiersIn(a.text, a.units[v].range)) {
        if (!id.member && (!item || id.item == *item) && spanText(a, id.span) == name)
          return true;
      }
      for (const HlslDecl &decl : a.units[v].scan.decls) {
        if (decl.name == name && decl.kind != DeclKind::Field)
          return true;
      }
    }
    return false;
  }
  bool nameInUse(const Analysis &a, const SymbolUses &uses, std::string_view name) {
    return nameInUse(a, uses.units, uses.item, uses.kind == SymbolUses::Kind::Property, name);
  }
  // ---- across files ------------------------------------------------------------
  // Whether a symbol is looked for in the files around the document: one declared in an include file is, when that
  // file is in the workspace or, for references alone, wherever it is.
  bool reachesWorkspace(const SymbolUses &uses, const Workspace &workspace) {
    return uses.declFile && (uses.kind == SymbolUses::Kind::External || workspace.contains(*uses.declFile));
  }
  // A symbol declared at file scope in `declFile` and named in one file that sees it.
  struct FileUses {
    std::shared_ptr<const Analysis> analysis;
    std::vector<size_t> units; // the code that sees the declaring file
    std::vector<Span> spans; // in document order
    std::optional<Span> declaration; // in the declaring file
    const MaterialProperty *property = nullptr; // the material property a shader binds to it
  };
  // The declaring file and every file in the workspace that includes it and names the symbol: in code that sees
  // the include, and as the material property of a shader whose variable it is.
  std::vector<FileUses> crossFileUses(const fs::path &declFile, const std::string &name, const Workspace &workspace, const FeatureContext &context) {
    std::string declKey = pathKey(declFile);
    std::vector<fs::path> files = workspace.filesMentioning(name);
    // With an index, only the files that can see the declaration are looked into: the ones that include the
    // declaring file, and the ones those include besides it, which are compiled after it - its siblings.
    std::set<std::string> siblings;
    if (auto includers = workspace.includers(declFile)) {
      std::set<std::string> reach{declKey};
      for (const fs::path &includer : *includers)
        reach.insert(pathKey(includer));
      std::vector<fs::path> roots = *includers;
      roots.push_back(declFile);
      for (const fs::path &sibling : workspace.included(roots).value_or(std::vector<fs::path>())) {
        if (reach.insert(pathKey(sibling)).second)
          siblings.insert(pathKey(sibling));
      }
      std::erase_if(files, [&](const fs::path &file) { return !reach.contains(pathKey(file)); });
    }
    if (std::none_of(files.begin(), files.end(), [&](const fs::path &file) { return pathKey(file) == declKey; }))
      files.push_back(declFile); // outside the workspace
    std::vector<FileUses> result;
    for (const fs::path &path : files) {
      FileUses uses{workspace.analysis(path)};
      if (!uses.analysis)
        continue;
      const Analysis &a = *uses.analysis;
      bool declaring = pathKey(path) == declKey;
      bool sibling = siblings.contains(pathKey(path));
      for (size_t u = 0; u < a.units.size(); ++u) {
        if (a.isGlsl(u))
          continue;
        bool sees = declaring || sibling;
        for (const auto &file : sees ? std::vector<std::shared_ptr<const CachedFile>>() : includedFiles(a, a.visibleUnits(u), context))
          sees = sees || pathKey(file->path) == declKey;
        if (sees)
          uses.units.push_back(u);
      }
      if (uses.units.empty())
        continue;
      for (size_t u : uses.units) {
        for (const Identifier &id : unitIdentifiers(a, u)) {
          if (!id.member && !id.local && spanText(a, id.span) == name)
            uses.spans.push_back(id.span);
        }
        for (const HlslDecl &decl : a.units[u].scan.decls) {
          if (declaring && !uses.declaration && decl.name == name && decl.kind != DeclKind::Field)
            uses.declaration = decl.nameSpan;
        }
      }
      uses.property = a.kind == DocumentKind::ShaderLab ? a.shader.findProperty(name) : nullptr;
      if (uses.property) {
        for (Span span : propertyUses(a, name, {}).spans)
          uses.spans.push_back(span);
      }
      sortSpans(uses.spans);
      result.push_back(std::move(uses));
    }
    return result;
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
    if (!a.isGlsl(*unit))
      return hlslCompletion(a, *unit, offset, context);
    return {{"isIncomplete", false}, {"items", json::array()}};
  }
  if (a.kind == DocumentKind::ShaderLab)
    return shaderLabCompletion(a, offset, context);
  return {{"isIncomplete", false}, {"items", json::array()}};
}
json hover(const Analysis &a, size_t offset, const FeatureContext &context) {
  if (auto unit = a.unitAt(offset))
    return a.isGlsl(*unit) ? nullptr : hlslHover(a, *unit, offset, context);
  if (a.kind == DocumentKind::ShaderLab)
    return shaderLabHover(a, offset, context);
  return nullptr;
}
json definition(const Analysis &a, size_t offset, const FeatureContext &context) {
  auto local = [&](Span span) { return location(a.path, a.text, a.lines, span, context.encoding); };
  if (auto unit = a.unitAt(offset)) {
    if (a.isGlsl(*unit))
      return nullptr;
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
    if (auto field = memberDecl(a, *unit, word, context)) {
      if (!field->file)
        return local(field->decl->nameSpan);
      return location(field->file->path, field->file->text, field->file->lines, field->decl->nameSpan, context.encoding);
    }
    if (auto declaration = localDecl(a, *unit, word))
      return local(wordAt(a.text, *declaration));
    std::vector<Guard> use = guardsAround(a, *unit, word.begin);
    if (const HlslDecl *decl = findLocalDecl(a, units, name, use))
      return local(decl->nameSpan);
    if (a.kind == DocumentKind::ShaderLab) {
      if (const MaterialProperty *property = a.shader.findProperty(name))
        return local(property->nameSpan);
    }
    if (hlsl::isKeywordOrType(name))
      return nullptr;
    if (auto external = findExternalDecl(includedFiles(a, units, context), name, use)) {
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
namespace {
  // UTF-16 code units in `text`, which is how a signature's parameters are marked in its label.
  size_t utf16Length(std::string_view text) {
    size_t length = 0;
    for (unsigned char c : text) {
      if ((c & 0xC0) != 0x80)
        length += c >= 0xF0 ? 2 : 1;
    }
    return length;
  }
  // A signature for the client: `label` such as "float3 Brighten(float3 c, float k)", with its parameters found
  // between the parentheses. Optional parameters written "[, int2 offset]", as the method tables do, count too.
  json signature(const std::string &label, const std::string &doc) {
    json parameters = json::array();
    size_t open = label.find('(');
    if (open != std::string::npos) {
      int depth = 0;
      size_t start = open + 1;
      for (size_t i = open + 1; i < label.size(); ++i) {
        char c = label[i];
        if (c == '(' || c == '<')
          ++depth;
        bool end = c == ')' && depth == 0;
        if ((c == ')' || c == '>') && depth > 0 && !end)
          --depth;
        if (end || (c == ',' && depth == 0)) {
          size_t b = start, e = i;
          while (b < e && (std::isspace(static_cast<unsigned char>(label[b])) || label[b] == '[' || label[b] == ']'))
            ++b;
          while (e > b && (std::isspace(static_cast<unsigned char>(label[e - 1])) || label[e - 1] == '[' || label[e - 1] == ']'))
            --e;
          if (e > b)
            parameters.push_back({{"label", {utf16Length(std::string_view(label).substr(0, b)), utf16Length(std::string_view(label).substr(0, e))}}});
          start = i + 1;
        }
        if (end)
          break;
      }
    }
    json result{{"label", label}, {"parameters", std::move(parameters)}};
    if (!doc.empty())
      result["documentation"] = markdown(doc);
    return result;
  }
  // Whether `query`'s letters appear in `name` in order, case aside: "brt" finds Brighten.
  bool fuzzyMatch(std::string_view name, std::string_view query) {
    size_t at = 0;
    for (char c : name) {
      if (at < query.size() && std::tolower(static_cast<unsigned char>(c)) == std::tolower(static_cast<unsigned char>(query[at])))
        ++at;
    }
    return at == query.size();
  }
} // namespace
json signatureHelp(const Analysis &a, size_t offset, const FeatureContext &context) {
  auto unit = a.unitAt(offset);
  if (!unit || a.isGlsl(*unit))
    return nullptr;
  // Back from the cursor to the '(' of the call it is in, counting the commas before it at that call's own level.
  std::vector<HlslToken> tokens = lexHlsl(a.text, {a.units[*unit].range.begin, offset});
  auto is = [&](size_t k, char c) { return tokens[k].kind == HlslTok::Punct && a.text[tokens[k].span.begin] == c; };
  int depth = 0;
  int active = 0;
  std::optional<size_t> open;
  for (size_t k = tokens.size(); k-- > 0;) {
    if (tokens[k].kind == HlslTok::Directive)
      return nullptr;
    if (is(k, ')') || is(k, ']'))
      ++depth;
    else if ((is(k, '(') || is(k, '[')) && depth > 0)
      --depth;
    else if (is(k, '(')) {
      open = k;
      break;
    } else if (depth == 0 && (is(k, ',')))
      ++active;
    else if (depth == 0 && (is(k, ';') || is(k, '{') || is(k, '}') || is(k, '[')))
      return nullptr;
  }
  if (!open || *open == 0 || tokens[*open - 1].kind != HlslTok::Ident)
    return nullptr;
  Span callee = tokens[*open - 1].span;
  std::string name = a.text.substr(callee.begin, callee.end - callee.begin);
  json signatures = json::array();
  auto dot = dotBefore(a.text, callee.begin, a.units[*unit].range.begin);
  if (dot) {
    TypeResolver resolver(a, *unit, context);
    auto type = resolver.typeBefore(*dot);
    for (const ref::Entry *method : type ? hlsl::methodsOf(type->name) : std::vector<const ref::Entry *>()) {
      if (method->name == name)
        signatures.push_back(signature(std::string(method->detail), std::string(method->doc)));
    }
  } else {
    std::vector<size_t> units = a.visibleUnits(*unit);
    for (bool activeCode : {true, false}) {
      for (size_t u : units) {
        for (const HlslDecl &decl : a.units[u].scan.decls) {
          if (decl.kind == DeclKind::Function && decl.name == name && decl.active == activeCode)
            signatures.push_back(signature(decl.detail, decl.doc));
        }
      }
    }
    if (signatures.empty()) {
      for (const auto &file : includedFiles(a, units, context)) {
        for (const HlslDecl &decl : file->scan.decls) {
          if (decl.kind == DeclKind::Function && decl.name == name)
            signatures.push_back(signature(decl.detail, decl.doc + (decl.doc.empty() ? "" : "\n\n") + "Defined in `" + displayPath(file->path) + "`"));
        }
      }
    }
    if (const ref::Entry *intrinsic = hlsl::findIntrinsic(name))
      signatures.push_back(signature(std::string(intrinsic->detail), std::string(intrinsic->doc)));
  }
  if (signatures.empty())
    return nullptr;
  // The first overload with room for the parameter the cursor is on.
  size_t chosen = 0;
  for (size_t i = 0; i < signatures.size(); ++i) {
    if (signatures[i]["parameters"].size() > static_cast<size_t>(active)) {
      chosen = i;
      break;
    }
  }
  return {{"signatures", std::move(signatures)}, {"activeSignature", chosen}, {"activeParameter", active}};
}
json workspaceSymbols(const std::string &query, const Workspace &workspace, const FeatureContext &context) {
  constexpr size_t kLimit = 1000;
  json result = json::array();
  if (const Index *index = workspace.index()) {
    for (const Index::Symbol &symbol : index->symbols(query, context.encoding, kLimit)) {
      std::string file = symbol.path.filename().string();
      json start{{"line", symbol.line}, {"character", symbol.column}};
      json end{{"line", symbol.line}, {"character", symbol.column + symbol.length}};
      int kind = symbol.kind == Index::kShaderName ? kSymClass : symbolKindFor(static_cast<DeclKind>(symbol.kind));
      result.push_back({{"name", symbol.name}, {"kind", kind}, {"location", {{"uri", workspace.uri(symbol.path)}, {"range", {{"start", start}, {"end", end}}}}},
        {"containerName", symbol.container.empty() ? file : symbol.container + " (" + file + ")"}});
    }
    return result;
  }
  for (const fs::path &path : workspace.files()) {
    auto a = workspace.analysis(path);
    if (!a)
      continue;
    std::string uri = workspace.uri(path);
    std::string file = path.filename().string();
    auto add = [&](const std::string &name, int kind, Span span, const std::string &container) {
      if (result.size() >= kLimit || !fuzzyMatch(name, query))
        return;
      // The client addresses an open document by the URI it opened it with.
      json where{{"uri", uri}, {"range", toRange(a->text, a->lines, span, context.encoding)}};
      result.push_back({{"name", name}, {"kind", kind}, {"location", std::move(where)}, {"containerName", container}});
    };
    if (a->kind == DocumentKind::ShaderLab && a->shader.hasShader && !a->shader.name.empty())
      add(a->shader.name, kSymClass, a->shader.nameSpan.empty() ? a->shader.keyword : a->shader.nameSpan, file);
    for (size_t u = 0; u < a->units.size(); ++u) {
      for (const HlslDecl &decl : a->isGlsl(u) ? std::vector<HlslDecl>() : a->units[u].scan.decls) {
        if (decl.kind != DeclKind::Field)
          add(decl.name, symbolKindFor(decl.kind), decl.nameSpan, decl.container.empty() ? file : decl.container + " (" + file + ")");
      }
    }
    if (result.size() >= kLimit)
      break;
  }
  return result;
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
json references(const Analysis &a, const std::string &uri, size_t offset, bool includeDeclaration, const FeatureContext &context, const Workspace &workspace) {
  auto uses = symbolUses(a, offset, context);
  if (!uses)
    return nullptr;
  json result = json::array();
  if (reachesWorkspace(*uses, workspace)) {
    for (const FileUses &file : crossFileUses(*uses->declFile, uses->name, workspace, context)) {
      const Analysis &f = *file.analysis;
      for (Span span : file.spans) {
        if (includeDeclaration || !file.declaration || span.begin != file.declaration->begin)
          result.push_back({{"uri", workspace.uri(f.path)}, {"range", toRange(f.text, f.lines, span, context.encoding)}});
      }
    }
    return result;
  }
  for (Span span : uses->spans) {
    if (includeDeclaration || !uses->declaration || span.begin != uses->declaration->begin)
      result.push_back({{"uri", uri}, {"range", toRange(a.text, a.lines, span, context.encoding)}});
  }
  return result;
}
json documentHighlights(const Analysis &a, size_t offset, const FeatureContext &context) {
  auto uses = symbolUses(a, offset, context);
  if (!uses)
    return nullptr;
  constexpr int kRead = 2, kWrite = 3; // DocumentHighlightKind
  json result = json::array();
  for (Span span : uses->spans) {
    bool declaration = uses->declaration && span.begin == uses->declaration->begin;
    result.push_back({{"range", toRange(a.text, a.lines, span, context.encoding)}, {"kind", declaration ? kWrite : kRead}});
  }
  return result;
}
namespace {
  // The symbol a rename starts from, refused when it can't be renamed safely.
  SymbolUses renamable(std::optional<SymbolUses> uses, const Workspace &workspace) {
    if (!uses)
      throw RequestRefused("There is no symbol here to rename.");
    // A symbol from an include file is renamed in that file and everything including it, when they are the user's.
    if (uses->kind == SymbolUses::Kind::External && uses->declFile && workspace.contains(*uses->declFile))
      uses->refusal.clear();
    if (!uses->refusal.empty())
      throw RequestRefused(uses->refusal);
    return std::move(*uses);
  }
  // The edits renaming `spans` in one file, and with a texture property the variables Unity binds by its name in
  // `units`.
  json renameEdits(const Analysis &a, const std::vector<size_t> &units, const std::vector<Span> &spans, bool texture, const std::string &from, const std::string &to, Encoding encoding) {
    json edits = json::array();
    auto edit = [&](Span span, const std::string &text) {
      edits.push_back({{"range", toRange(a.text, a.lines, span, encoding)}, {"newText", text}});
    };
    for (Span span : spans)
      edit(span, to);
    if (texture) {
      auto companions = textureCompanions(from, to);
      for (size_t v : units) {
        for (const Identifier &id : unitIdentifiers(a, v)) {
          std::string text = spanText(a, id.span);
          for (const auto &[old, renamed] : companions) {
            if (!id.member && !id.local && text == old)
              edit(id.span, renamed);
          }
        }
      }
    }
    return edits;
  }
} // namespace
json prepareRename(const Analysis &a, size_t offset, const FeatureContext &context, const Workspace &workspace) {
  auto found = symbolUses(a, offset, context);
  if (!found)
    return nullptr;
  SymbolUses uses = renamable(std::move(found), workspace);
  return {{"range", toRange(a.text, a.lines, uses.at, context.encoding)}, {"placeholder", uses.name}};
}
json rename(const Analysis &a, const std::string &uri, size_t offset, const std::string &newName, const FeatureContext &context, const Workspace &workspace) {
  SymbolUses uses = renamable(symbolUses(a, offset, context), workspace);
  if (newName.empty() || !isIdentStart(newName[0]) || !std::all_of(newName.begin(), newName.end(), isIdentChar))
    throw RequestRefused("'" + newName + "' is not a valid identifier.");
  if (isBuiltinName(newName))
    throw RequestRefused(newName + " is an HLSL keyword, type or intrinsic.");
  auto inUse = [&] {
    return RequestRefused(newName + " is already in use here; renaming " + uses.name + " to it would change what the code means.");
  };
  json changes = json::object();
  if (newName == uses.name)
    return {{"changes", changes}};
  if (reachesWorkspace(uses, workspace)) {
    std::vector<FileUses> files = crossFileUses(*uses.declFile, uses.name, workspace, context);
    bool texture = false;
    for (const FileUses &file : files) {
      if (nameInUse(*file.analysis, file.units, std::nullopt, file.property != nullptr, newName))
        throw inUse();
      texture = texture || (file.property && isTextureProperty(*file.property));
    }
    for (const FileUses &file : files)
      changes[workspace.uri(file.analysis->path)] = renameEdits(*file.analysis, file.units, file.spans, texture, uses.name, newName, context.encoding);
    return {{"changes", changes}};
  }
  if (nameInUse(a, uses, newName))
    throw inUse();
  const MaterialProperty *property = uses.kind == SymbolUses::Kind::Property ? a.shader.findProperty(uses.name) : nullptr;
  changes[uri] = renameEdits(a, uses.units, uses.spans, property && isTextureProperty(*property), uses.name, newName, context.encoding);
  return {{"changes", changes}};
}
} // namespace sls
