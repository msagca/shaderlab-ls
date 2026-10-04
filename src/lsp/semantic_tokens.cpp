#include "lsp/semantic_tokens.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include "common/util.h"
#include "hlsl/builtins.h"
#include "hlsl/lexer.h"
#include "shaderlab/lexer.h"
#include "shaderlab/reference.h"
using json = nlohmann::json;
namespace sls {
namespace {
  // The legend's order: a token's type is its index here.
  enum TokenType {
    kNamespace,
    kType,
    kStruct,
    kParameter,
    kVariable,
    kProperty,
    kEnumMember,
    kFunction,
    kMethod,
    kMacro,
    kKeyword,
    kComment,
    kString,
    kNumber,
    kDecorator,
  };
  constexpr const char *kTypeNames[] = {"namespace", "type", "struct", "parameter", "variable", "property", "enumMember", "function", "method", "macro", "keyword", "comment", "string", "number", "decorator"};
  // Bit flags, in the legend's order.
  enum TokenModifier {
    kDeclaration = 1,
    kDefaultLibrary = 2,
  };
  constexpr const char *kModifierNames[] = {"declaration", "defaultLibrary"};
  struct SemanticToken {
    Span span;
    int type;
    int modifiers = 0;
  };
  bool allCaps(std::string_view name) {
    return name.size() > 1 && std::any_of(name.begin(), name.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); }) &&
           std::none_of(name.begin(), name.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); });
  }
  bool isStagePragma(std::string_view name) {
    return name == "vertex" || name == "fragment" || name == "geometry" || name == "hull" || name == "domain" ||
           name == "kernel" || name == "surface";
  }
  // Declaration kinds by how they rank when one name has several: a function before a struct before a variable.
  int rank(DeclKind kind) {
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
  int typeFor(DeclKind kind) {
    switch (kind) {
    case DeclKind::Function:
      return kFunction;
    case DeclKind::Struct:
      return kStruct;
    case DeclKind::CBuffer:
      return kNamespace;
    case DeclKind::Variable:
      return kVariable;
    case DeclKind::Macro:
      return kMacro;
    case DeclKind::Field:
      return kProperty;
    }
    return kVariable;
  }
  class Collector {
  public:
    Collector(const Analysis &a, const FeatureContext &context)
      : a_(a), context_(context) {}
    json run() {
      if (a_.kind == DocumentKind::ShaderLab)
        shaderLab();
      declarations();
      for (size_t u = 0; u < a_.units.size(); ++u) {
        if (a_.isGlsl(u))
          glsl(a_.units[u].range);
        else
          hlsl(u);
      }
      return encode();
    }
  private:
    std::string_view text(Span span) const {
      return std::string_view(a_.text).substr(span.begin, span.end - span.begin);
    }
    void add(Span span, int type, int modifiers = 0) {
      if (!span.empty())
        tokens_.push_back({span, type, modifiers});
    }
    // ---- ShaderLab ---------------------------------------------------------
    void shaderLab() {
      const ShaderFile &shader = a_.shader;
      // Names the parser has placed, by where they start.
      std::map<size_t, std::pair<int, int>> placed;
      for (const MaterialProperty &property : shader.properties) {
        placed[property.nameSpan.begin] = {kProperty, kDeclaration};
        placed[property.typeSpan.begin] = {kType, 0};
        for (const PropertyAttribute &attribute : property.attributes)
          placed[attribute.nameSpan.begin] = {kDecorator, 0};
      }
      auto commands = [&](const std::vector<Command> &list, auto &self) -> void {
        for (const Command &command : list) {
          for (const CommandArg &arg : command.args) {
            if (!arg.comma)
              placed[arg.span.begin] = {arg.propertyRef ? kProperty : kEnumMember, 0};
          }
          self(command.children, self);
        }
      };
      for (const SubShader &subShader : shader.subShaders) {
        commands(subShader.commands, commands);
        for (const Pass &pass : subShader.passes)
          commands(pass.commands, commands);
      }
      std::vector<Span> properties;
      for (const Scope &scope : shader.scopes) {
        if (scope.kind == ScopeKind::Properties)
          properties.push_back(scope.span);
      }
      std::vector<Token> lexed = lexShaderLab(a_.text, true);
      for (size_t k = 0; k < lexed.size(); ++k) {
        const Token &token = lexed[k];
        switch (token.kind) {
        case Tok::Comment:
          add(token.span, kComment);
          break;
        case Tok::String:
          add(token.span, kString);
          break;
        case Tok::Number:
          add(token.span, kNumber);
          break;
        case Tok::CodeBlock:
          add({token.span.begin, token.content.begin}, kKeyword);
          add(token.endKeyword, kKeyword);
          break;
        case Tok::Ident: {
          if (auto it = placed.find(token.span.begin); it != placed.end()) {
            add(token.span, it->second.first, it->second.second);
            break;
          }
          // [_Prop] that the parser skipped, in a fixed-function command; an attribute inside Properties.
          bool bracketed = k > 0 && lexed[k - 1].kind == Tok::LBracket && k + 1 < lexed.size() && lexed[k + 1].kind == Tok::RBracket;
          if (bracketed) {
            bool attribute = std::any_of(properties.begin(), properties.end(), [&](Span scope) { return token.span.begin > scope.begin && token.span.begin < scope.end; });
            add(token.span, attribute ? kDecorator : kProperty);
            break;
          }
          std::string_view name = token.text;
          if (iequals(name, "Shader") || ref::findKeywordAnywhere(name) || ref::findCommand(name) || ref::isScopeKeyword(name) ||
              ref::isLegacyCommand(name) || ref::find(ref::stencilFields(), name) || ref::find(ref::legacySubCommands(), name)) {
            add(token.span, kKeyword);
          } else if (ref::find(ref::onOff(), name) || ref::find(ref::trueFalse(), name) || ref::find(ref::cullModes(), name) ||
                     ref::find(ref::zTestOperations(), name) || ref::find(ref::blendFactors(), name)) {
            add(token.span, kEnumMember);
          }
          break;
        }
        default:
          break;
        }
      }
    }
    // ---- HLSL --------------------------------------------------------------
    // What every name declared in the document or its includes is, for telling a function from a variable.
    void declarations() {
      std::vector<size_t> code;
      for (size_t u = 0; u < a_.units.size(); ++u) {
        if (a_.isGlsl(u))
          continue;
        code.push_back(u);
        for (const HlslDecl &decl : a_.units[u].scan.decls) {
          declare(decl);
          declared_.insert(decl.nameSpan.begin);
        }
      }
      if (!code.empty()) {
        for (const auto &file : includedFiles(a_, code, context_.editorOverride)) {
          for (const HlslDecl &decl : file->scan.decls)
            declare(decl);
        }
      }
    }
    void declare(const HlslDecl &decl) {
      auto [it, inserted] = kinds_.try_emplace(decl.name, decl.kind);
      if (!inserted && rank(decl.kind) < rank(it->second))
        it->second = decl.kind;
    }
    // A name by what it is anywhere: a keyword, a type, a declaration of the document or its includes, an intrinsic.
    std::optional<std::pair<int, int>> classify(std::string_view name, size_t begin) const {
      if (hlsl::findKeyword(name))
        return std::pair{kKeyword, 0};
      if (hlsl::isKeywordOrType(name) || hlsl::numericShape(name))
        return std::pair{kType, kDefaultLibrary};
      if (auto it = kinds_.find(std::string(name)); it != kinds_.end())
        return std::pair{typeFor(it->second), declared_.contains(begin) ? kDeclaration : 0};
      if (hlsl::findIntrinsic(name))
        return std::pair{kFunction, kDefaultLibrary};
      if (allCaps(name))
        return std::pair{kMacro, 0};
      return std::nullopt;
    }
    void hlsl(size_t unit) {
      std::vector<HlslToken> tokens = lexHlsl(a_.text, a_.units[unit].range);
      auto punct = [&](size_t i, char c) {
        return i < tokens.size() && tokens[i].kind == HlslTok::Punct && a_.text[tokens[i].span.begin] == c;
      };
      // Where the walk is: a function's parameter list, its body, or neither.
      int depth = 0;
      int parens = 0;
      bool inParameters = false;
      bool signature = false; // a parameter list closed at file scope, its body not opened yet
      bool body = false;
      std::set<std::string, std::less<>> parameters, locals;
      for (size_t k = 0; k < tokens.size(); ++k) {
        const HlslToken &token = tokens[k];
        switch (token.kind) {
        case HlslTok::Comment:
          add(token.span, kComment);
          continue;
        case HlslTok::String:
          add(token.span, kString);
          continue;
        case HlslTok::Number:
          add(token.span, kNumber);
          continue;
        case HlslTok::Directive:
          directive(unit, token.span);
          continue;
        case HlslTok::Punct: {
          char c = a_.text[token.span.begin];
          if (c == '(') {
            if (depth == 0 && parens == 0 && k > 0 && tokens[k - 1].kind == HlslTok::Ident) {
              inParameters = true;
              parameters.clear();
              locals.clear();
            }
            ++parens;
          } else if (c == ')' && parens > 0 && --parens == 0 && inParameters) {
            inParameters = false;
            signature = true;
          } else if (c == '{') {
            body = body || (depth == 0 && signature);
            signature = false;
            ++depth;
          } else if (c == '}' && depth > 0 && --depth == 0) {
            body = false;
          } else if (c == ';' && depth == 0) {
            signature = inParameters = false;
            parens = 0;
          }
          continue;
        }
        case HlslTok::Ident:
          break;
        }
        std::string_view name = text(token.span);
        if (k > 0 && punct(k - 1, '.')) {
          add(token.span, punct(k + 1, '(') ? kMethod : kProperty);
          continue;
        }
        if (k > 0 && punct(k - 1, ':') && hlsl::findSemantic(name)) {
          add(token.span, kDecorator);
          continue;
        }
        if (k > 0 && punct(k - 1, '[') && hlsl::findAttribute(name) && !kinds_.contains(std::string(name))) {
          add(token.span, kDecorator);
          continue;
        }
        // A parameter or local being declared: a type before it, and a separator, an initializer or a semantic after.
        bool typed = k > 0 && ((tokens[k - 1].kind == HlslTok::Ident && !hlsl::findKeyword(text(tokens[k - 1].span))) || punct(k - 1, '>'));
        bool declarator = typed && (punct(k + 1, ',') || punct(k + 1, ')') || punct(k + 1, ';') || punct(k + 1, '=') || punct(k + 1, ':') || punct(k + 1, '['));
        if (declarator && inParameters && depth == 0) {
          parameters.emplace(name);
          add(token.span, kParameter, kDeclaration);
          continue;
        }
        if (declarator && body) {
          locals.emplace(name);
          add(token.span, kVariable, kDeclaration);
          continue;
        }
        if (body && locals.contains(name)) {
          add(token.span, kVariable);
          continue;
        }
        if (body && parameters.contains(name)) {
          add(token.span, kParameter);
          continue;
        }
        if (auto kind = classify(name, token.span.begin))
          add(token.span, kind->first, kind->second);
      }
    }
    void directive(size_t unit, Span span) {
      size_t i = span.begin + 1;
      while (i < span.end && (a_.text[i] == ' ' || a_.text[i] == '\t'))
        ++i;
      size_t wordBegin = i;
      while (i < span.end && isIdentChar(a_.text[i]))
        ++i;
      std::string_view word = text({wordBegin, i});
      add({span.begin, i}, kKeyword);
      const HlslScan &scan = a_.units[unit].scan;
      if (word == "include" || word == "include_with_pragmas") {
        for (const HlslInclude &include : scan.includes) {
          if (include.span.begin == span.begin)
            add({std::max(include.pathSpan.begin, span.begin + 1) - 1, std::min(include.pathSpan.end + 1, span.end)}, kString);
        }
        return;
      }
      if (word == "pragma") {
        for (const HlslPragma &pragma : scan.pragmas) {
          if (pragma.span.begin != span.begin)
            continue;
          add(pragma.nameSpan, kKeyword);
          bool keywords = ref::isKeywordPragma(pragma.name);
          for (const PragmaArg &arg : pragma.args) {
            if (arg.text.starts_with("//"))
              break;
            if (isStagePragma(pragma.name))
              add(arg.span, kFunction);
            else if (keywords)
              add(arg.span, arg.text.find_first_not_of('_') == std::string::npos ? kKeyword : kMacro);
            else
              add(arg.span, std::isdigit(static_cast<unsigned char>(arg.text[0])) ? kNumber : kEnumMember);
          }
        }
        return;
      }
      bool name = word == "define" || word == "undef" || word == "ifdef" || word == "ifndef";
      for (const HlslToken &token : lexHlsl(a_.text, {i, span.end})) {
        switch (token.kind) {
        case HlslTok::Comment:
          add(token.span, kComment);
          break;
        case HlslTok::String:
          add(token.span, kString);
          break;
        case HlslTok::Number:
          add(token.span, kNumber);
          break;
        case HlslTok::Ident: {
          std::string_view identifier = text(token.span);
          if (name) {
            add(token.span, kMacro, word == "define" ? kDeclaration : 0);
            name = false;
          } else if (identifier == "defined") {
            add(token.span, kKeyword);
          } else if (auto kind = classify(identifier, token.span.begin)) {
            add(token.span, kind->first, kind->second);
          } else if (word != "define") {
            add(token.span, kMacro); // a condition names macros
          }
          break;
        }
        default:
          break;
        }
      }
    }
    // ---- GLSL --------------------------------------------------------------
    void glsl(Span range) {
      for (const HlslToken &token : lexHlsl(a_.text, range)) {
        if (token.kind == HlslTok::Comment) {
          add(token.span, kComment);
        } else if (token.kind == HlslTok::String) {
          add(token.span, kString);
        } else if (token.kind == HlslTok::Number) {
          add(token.span, kNumber);
        } else if (token.kind == HlslTok::Directive) {
          size_t i = token.span.begin + 1;
          while (i < token.span.end && (a_.text[i] == ' ' || a_.text[i] == '\t'))
            ++i;
          while (i < token.span.end && isIdentChar(a_.text[i]))
            ++i;
          add({token.span.begin, i}, kKeyword);
        }
      }
    }
    // ---- encoding ----------------------------------------------------------
    // LSP's relative integers, five to a token, one token per line: a comment over three lines is three tokens.
    json encode() {
      std::stable_sort(tokens_.begin(), tokens_.end(), [](const SemanticToken &x, const SemanticToken &y) { return x.span.begin < y.span.begin; });
      std::vector<int> data;
      int lastLine = 0, lastCharacter = 0;
      size_t covered = 0;
      for (const SemanticToken &token : tokens_) {
        if (token.span.begin < covered)
          continue; // overlaps the one before
        covered = token.span.end;
        size_t begin = token.span.begin;
        while (begin < token.span.end) {
          size_t line = a_.lines.lineOf(begin);
          size_t end = std::min(token.span.end, a_.lines.lineEnd(a_.text, line));
          if (end > begin) {
            Position start = a_.lines.toPosition(a_.text, begin, context_.encoding);
            Position stop = a_.lines.toPosition(a_.text, end, context_.encoding);
            data.insert(data.end(), {start.line - lastLine, start.line == lastLine ? start.character - lastCharacter : start.character, stop.character - start.character, token.type, token.modifiers});
            lastLine = start.line;
            lastCharacter = start.character;
          }
          if (line + 1 >= a_.lines.lineCount())
            break;
          begin = a_.lines.lineStart(line + 1);
        }
      }
      return {{"data", std::move(data)}};
    }
    const Analysis &a_;
    const FeatureContext &context_;
    std::vector<SemanticToken> tokens_;
    std::unordered_map<std::string, DeclKind> kinds_;
    std::set<size_t> declared_; // where the document's own declarations name what they declare
  };
} // namespace
json semanticTokensLegend() {
  return {{"tokenTypes", kTypeNames}, {"tokenModifiers", kModifierNames}};
}
json semanticTokens(const Analysis &analysis, const FeatureContext &context) {
  return Collector(analysis, context).run();
}
} // namespace sls
