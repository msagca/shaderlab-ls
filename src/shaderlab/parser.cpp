#include "shaderlab/parser.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <string>
#include "common/util.h"
#include "shaderlab/lexer.h"
#include "shaderlab/reference.h"
namespace sls {
const MaterialProperty *ShaderFile::findProperty(std::string_view propertyName) const {
  for (const MaterialProperty &property : properties) {
    if (property.name == propertyName)
      return &property;
  }
  return nullptr;
}
namespace {
  std::string joinNames(std::span<const ref::Entry> table) {
    std::string result;
    size_t count = 0;
    for (const ref::Entry &entry : table) {
      if (count == 12) {
        result += ", ...";
        break;
      }
      if (!result.empty())
        result += ", ";
      result += entry.name;
      ++count;
    }
    return result;
  }
  std::optional<long> parseInteger(std::string_view text) {
    long value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size())
      return std::nullopt;
    return value;
  }
  using ref::isLegacyCommand;
  using ref::isScopeKeyword;
  class Parser {
  public:
    explicit Parser(std::string_view source)
      : src_(source), toks_(lexShaderLab(source)), lines_(source) {}
    ShaderFile run() {
      size_t lastJunkLine = static_cast<size_t>(-1);
      while (!atEnd()) {
        const Token &token = peek();
        if (isWord(token, "Shader")) {
          if (out_.hasShader)
            report(token.span, "Only one Shader block is allowed in a file.");
          parseShader();
          continue;
        }
        if (lineOf(token) != lastJunkLine) {
          report(token.span, token.kind == Tok::CodeBlock ? "Shader code blocks must be inside a Shader block." : "Expected 'Shader \"<name>\" { ... }'.");
          lastJunkLine = lineOf(token);
        }
        advance();
      }
      if (!out_.hasShader && out_.diagnostics.empty()) {
        report({0, 0}, "Expected 'Shader \"<name>\" { ... }'.");
      }
      validatePropertyReferences();
      return std::move(out_);
    }
  private:
    // ---- token helpers -------------------------------------------------------
    const Token &peek(size_t ahead = 0) const {
      return toks_[std::min(pos_ + ahead, toks_.size() - 1)];
    }
    const Token &advance() {
      const Token &token = toks_[pos_];
      if (token.kind != Tok::End) {
        lastEnd_ = token.span.end;
        ++pos_;
      }
      return token;
    }
    bool atEnd() const {
      return peek().kind == Tok::End;
    }
    bool isWord(const Token &token, std::string_view word) const {
      return token.kind == Tok::Ident && iequals(token.text, word);
    }
    size_t lineOf(const Token &token) const {
      return lines_.lineOf(token.span.begin);
    }
    void report(Span span, std::string message, Severity severity = Severity::Error) {
      Diagnostic diagnostic;
      diagnostic.span = span;
      diagnostic.severity = severity;
      diagnostic.message = std::move(message);
      out_.diagnostics.push_back(std::move(diagnostic));
    }
    void reportUnexpected(const Token &token, std::string_view context) {
      if (token.kind == Tok::Unknown) {
        report(token.span, "Unexpected character '" + std::string(token.text) + "'.");
      } else if (token.kind == Tok::End) {
        report(token.span, "Unexpected end of file.");
      } else {
        report(token.span, "'" + std::string(token.text.substr(0, 40)) + "' is not valid inside " + std::string(context) + ".");
      }
    }
    // Consumes the rest of a one-line statement, including any { } block it opens.
    void skipStatement(const Token &start) {
      size_t line = lineOf(start);
      bool advanced = false;
      while (!atEnd()) {
        const Token &token = peek();
        if (advanced && lineOf(token) != line)
          break;
        if (token.kind == Tok::RBrace)
          break;
        if (advanced && token.kind == Tok::CodeBlock)
          break;
        if (token.kind == Tok::LBrace) {
          skipBalanced();
          advanced = true;
          continue;
        }
        advance();
        advanced = true;
      }
    }
    void skipBalanced() {
      int depth = 0;
      while (!atEnd()) {
        const Token &token = advance();
        if (token.kind == Tok::LBrace)
          ++depth;
        if (token.kind == Tok::RBrace && --depth <= 0)
          return;
      }
    }
    std::optional<std::string> expectString(const Token &anchor, std::string_view what) {
      const Token &token = peek();
      if (token.kind == Tok::String) {
        advance();
        if (!token.stringTerminated)
          report(token.span, "Unterminated string.");
        return stringValue(token);
      }
      report(lineOf(token) == lineOf(anchor) && token.kind != Tok::End ? token.span : anchor.span,
        "Expected " + std::string(what) + ".");
      return std::nullopt;
    }
    bool openBrace(const Token &keyword, ScopeKind kind, ScopeKind parent, size_t &scopeIndex) {
      const Token &token = peek();
      if (token.kind != Tok::LBrace) {
        report(keyword.span, "Expected '{' after '" + std::string(keyword.text) + "'.");
        return false;
      }
      advance();
      scopeIndex = out_.scopes.size();
      out_.scopes.push_back({kind, parent, {token.span.begin, src_.size()}});
      return true;
    }
    void closeBrace(const Token &keyword, size_t scopeIndex) {
      if (peek().kind == Tok::RBrace) {
        const Token &close = advance();
        out_.scopes[scopeIndex].span.end = close.span.end;
        return;
      }
      report(keyword.span, "Missing '}' to close '" + std::string(keyword.text) + "'.");
    }
    // ---- blocks --------------------------------------------------------------
    void recordBlock(const Token &token, int subShader, int pass) {
      CodeBlock block;
      block.kind = token.block;
      block.keyword = {token.span.begin, token.span.begin + std::strlen(blockKeyword(token.block))};
      block.content = token.content;
      block.endKeyword = token.endKeyword;
      block.subShader = subShader;
      block.pass = pass;
      if (!token.terminated) {
        const char *terminator = isCgBlock(token.block) ? "ENDCG" : token.block == BlockKind::GlslProgram ? "ENDGLSL"
                                                                                                          : "ENDHLSL";
        report(block.keyword, std::string("Missing ") + terminator + " for " + blockKeyword(token.block) + ".");
      }
      if (token.block == BlockKind::GlslProgram) {
        report(block.keyword, "GLSLPROGRAM blocks are not analyzed; only HLSL is supported.", Severity::Information);
      }
      out_.blocks.push_back(block);
    }
    void parseShader() {
      const Token &keyword = advance();
      out_.hasShader = true;
      out_.keyword = keyword.span;
      if (peek().kind == Tok::String) {
        const Token &name = advance();
        if (!name.stringTerminated)
          report(name.span, "Unterminated string.");
        out_.name = stringValue(name);
        out_.nameSpan = name.span;
      } else {
        report(keyword.span, "Expected a shader name string after 'Shader'.");
      }
      size_t scope = 0;
      if (!openBrace(keyword, ScopeKind::Shader, ScopeKind::Shader, scope))
        return;
      while (!atEnd() && peek().kind != Tok::RBrace) {
        const Token &token = peek();
        if (token.kind == Tok::CodeBlock) {
          recordBlock(token, -1, -1);
          if (isProgramBlock(token.block)) {
            report(out_.blocks.back().keyword, std::string(blockKeyword(token.block)) + " must be inside a Pass or SubShader.");
          }
          advance();
        } else if (isWord(token, "Properties")) {
          parseProperties();
        } else if (isWord(token, "SubShader")) {
          parseSubShader();
        } else if (isWord(token, "CustomEditor")) {
          const Token &kw = advance();
          expectString(kw, "a custom editor class name string");
        } else if (isWord(token, "CustomEditorForRenderPipeline")) {
          const Token &kw = advance();
          if (expectString(kw, "a custom editor class name string")) {
            expectString(kw, "a render pipeline asset class name string");
          }
        } else if (isWord(token, "Fallback")) {
          const Token &kw = advance();
          if (peek().kind == Tok::String) {
            out_.fallback = stringValue(advance());
          } else if (isWord(peek(), "Off")) {
            advance();
          } else {
            report(kw.span, "Expected a shader name string or 'Off' after 'Fallback'.");
          }
        } else if (isWord(token, "Dependency")) {
          // Not in the current reference; used by Unity's terrain and SpeedTree shaders.
          const Token &kw = advance();
          if (expectString(kw, "a dependency name string")) {
            if (peek().kind == Tok::Equals) {
              advance();
              expectString(kw, "a shader name string");
            } else {
              report(kw.span, "Expected '=' after the dependency name.");
            }
          }
        } else {
          reportUnexpected(token, "a Shader block");
          skipStatement(token);
        }
      }
      closeBrace(keyword, scope);
      out_.span = {keyword.span.begin, lastEnd_};
    }
    // ---- Properties ----------------------------------------------------------
    void parseProperties() {
      const Token &keyword = advance();
      if (!out_.propertiesKeyword.empty())
        report(keyword.span, "Duplicate Properties block.", Severity::Warning);
      out_.propertiesKeyword = keyword.span;
      size_t scope = 0;
      if (!openBrace(keyword, ScopeKind::Properties, ScopeKind::Shader, scope))
        return;
      while (!atEnd() && peek().kind != Tok::RBrace) {
        size_t before = pos_;
        parseProperty();
        if (pos_ == before)
          advance();
      }
      closeBrace(keyword, scope);
    }
    void skipLine(size_t line) {
      while (!atEnd() && peek().kind != Tok::RBrace && lineOf(peek()) == line) {
        if (peek().kind == Tok::LBrace) {
          skipBalanced();
        } else {
          advance();
        }
      }
    }
    void parseProperty() {
      MaterialProperty property;
      size_t start = peek().span.begin;
      while (peek().kind == Tok::LBracket) {
        const Token &open = advance();
        size_t close = std::string_view::npos;
        int depth = 0;
        for (size_t i = open.span.end; i < src_.size() && src_[i] != '\n'; ++i) {
          if (src_[i] == '(')
            ++depth;
          if (src_[i] == ')')
            --depth;
          if (src_[i] == ']' && depth <= 0) {
            close = i;
            break;
          }
        }
        if (close == std::string_view::npos) {
          report(open.span, "Unterminated attribute: missing ']'.");
          skipLine(lineOf(open));
          return;
        }
        PropertyAttribute attribute;
        std::string_view inner = src_.substr(open.span.end, close - open.span.end);
        size_t paren = inner.find('(');
        std::string_view name = trim(inner.substr(0, paren));
        attribute.name = std::string(name);
        if (!name.empty()) {
          size_t nameBegin = static_cast<size_t>(name.data() - src_.data());
          attribute.nameSpan = {nameBegin, nameBegin + name.size()};
        }
        if (paren != std::string_view::npos) {
          std::string_view args = inner.substr(paren + 1);
          size_t last = args.rfind(')');
          attribute.arguments = std::string(trim(args.substr(0, last)));
        }
        attribute.span = {open.span.begin, close + 1};
        if (name.empty())
          report(attribute.span, "Empty property attribute.");
        property.attributes.push_back(std::move(attribute));
        while (!atEnd() && peek().span.begin <= close)
          advance();
      }
      const Token &nameToken = peek();
      if (nameToken.kind != Tok::Ident) {
        reportUnexpected(nameToken, "a Properties block (expected a material property declaration)");
        skipLine(lineOf(nameToken));
        return;
      }
      advance();
      property.name = std::string(nameToken.text);
      property.nameSpan = nameToken.span;
      size_t line = lineOf(nameToken);
      auto fail = [&](const Token &at, const std::string &message) {
        report(at.kind == Tok::End ? nameToken.span : at.span, message);
        skipLine(line);
      };
      if (peek().kind != Tok::LParen)
        return fail(peek(), "Expected '(' after property name '" + property.name + "'.");
      advance();
      if (peek().kind != Tok::String)
        return fail(peek(), "Expected a display name string.");
      property.displayName = stringValue(advance());
      if (peek().kind != Tok::Comma)
        return fail(peek(), "Expected ',' after the display name.");
      advance();
      const Token &typeToken = peek();
      if (typeToken.kind != Tok::Ident)
        return fail(typeToken, "Expected a property type.");
      advance();
      property.type = std::string(typeToken.text);
      property.typeSpan = typeToken.span;
      const ref::Entry *typeEntry = ref::find(ref::propertyTypes(), property.type);
      if (!typeEntry) {
        report(typeToken.span, "Unknown property type '" + property.type + "'. Expected one of: " + joinNames(ref::propertyTypes()) + ".");
      }
      if (iequals(property.type, "Range")) {
        bool ok = peek().kind == Tok::LParen && peek(1).kind == Tok::Number && peek(2).kind == Tok::Comma &&
                  peek(3).kind == Tok::Number && peek(4).kind == Tok::RParen;
        if (!ok)
          return fail(peek(), "Expected 'Range(min, max)'.");
        for (int i = 0; i < 5; ++i)
          advance();
      } else if (iequals(property.type, "Vector") && peek().kind == Tok::Comma && peek(1).kind == Tok::Number) {
        advance();
        const Token &count = advance();
        if (count.text != "2" && count.text != "3" && count.text != "4") {
          report(count.span, "The number of Vector fields to display must be 2, 3, or 4.");
        }
      }
      if (peek().kind != Tok::RParen)
        return fail(peek(), "Expected ')' after the property type.");
      advance();
      if (peek().kind != Tok::Equals)
        return fail(peek(), "Expected '=' and a default value.");
      advance();
      enum class Default { None,
        Number,
        Tuple,
        Texture } defaultKind = Default::None;
      const Token &valueToken = peek();
      size_t defaultBegin = valueToken.span.begin;
      if (valueToken.kind == Tok::Number) {
        advance();
        defaultKind = Default::Number;
      } else if (valueToken.kind == Tok::LParen) {
        advance();
        while (!atEnd() && (peek().kind == Tok::Number || peek().kind == Tok::Comma))
          advance();
        if (peek().kind != Tok::RParen)
          return fail(peek(), "Expected ')' to close the default value.");
        advance();
        defaultKind = Default::Tuple;
      } else if (valueToken.kind == Tok::String) {
        advance();
        defaultKind = Default::Texture;
        if (peek().kind == Tok::LBrace)
          skipBalanced();
      } else {
        return fail(valueToken, "Expected a default value.");
      }
      property.defaultValue = std::string(src_.substr(defaultBegin, lastEnd_ - defaultBegin));
      if (typeEntry) {
        std::string_view type = typeEntry->name;
        bool numeric = type == "Integer" || type == "Int" || type == "Float" || type == "Range";
        bool vector = type == "Color" || type == "Vector";
        Default expected = numeric ? Default::Number : vector ? Default::Tuple
                                                              : Default::Texture;
        if (defaultKind != expected) {
          const char *shape = numeric  ? "a number, for example 0.5"
                              : vector ? "four numbers, for example (1, 1, 1, 1)"
                                       : "a texture name string, for example \"white\" {}";
          report({defaultBegin, lastEnd_}, "The default value of a " + std::string(type) + " property must be " + shape + ".");
        }
      }
      property.span = {start, lastEnd_};
      if (out_.findProperty(property.name)) {
        report(property.nameSpan, "Duplicate material property '" + property.name + "'.", Severity::Warning);
      }
      out_.properties.push_back(std::move(property));
    }
    // ---- SubShader / Pass ----------------------------------------------------
    void parseSubShader() {
      const Token &keyword = advance();
      int index = static_cast<int>(out_.subShaders.size());
      out_.subShaders.emplace_back();
      out_.subShaders.back().keyword = keyword.span;
      size_t scope = 0;
      if (!openBrace(keyword, ScopeKind::SubShader, ScopeKind::Shader, scope))
        return;
      bool first = true;
      while (!atEnd() && peek().kind != Tok::RBrace) {
        const Token &token = peek();
        if (token.kind == Tok::CodeBlock) {
          recordBlock(token, index, -1);
          advance();
        } else if (isWord(token, "PackageRequirements")) {
          if (!first)
            report(token.span, "A PackageRequirements block must come before all other declarations inside the SubShader.");
          parsePackageRequirements(ScopeKind::SubShader);
        } else if (isWord(token, "LOD")) {
          const Token &kw = advance();
          auto value = peek().kind == Tok::Number ? parseInteger(peek().text) : std::nullopt;
          if (value) {
            advance();
            out_.subShaders[index].lod = *value;
          } else {
            report(kw.span, "Expected an integer LOD value.");
            skipStatement(kw);
          }
        } else if (isWord(token, "Tags")) {
          parseTags(index, -1);
        } else if (isWord(token, "Pass")) {
          parsePass(index);
        } else if (isWord(token, "UsePass")) {
          const Token &kw = advance();
          expectString(kw, "\"Shader object name/PASS NAME IN UPPERCASE\"");
        } else if (isWord(token, "GrabPass")) {
          parseGrabPass();
        } else if (token.kind == Tok::Ident && ref::findCommand(token.text)) {
          parseCommand(out_.subShaders[index].commands, ScopeKind::SubShader);
        } else if (isWord(token, "Name")) {
          report(token.span, "'Name' is only valid inside a Pass.");
          skipStatement(token);
        } else if (token.kind == Tok::Ident && isLegacyCommand(token.text)) {
          skipLegacyCommand();
        } else if (token.kind == Tok::Ident) {
          report(token.span, "'" + std::string(token.text) + "' is not a ShaderLab command in the current reference.", Severity::Warning);
          skipStatement(token);
        } else {
          reportUnexpected(token, "a SubShader block");
          skipStatement(token);
        }
        first = false;
      }
      closeBrace(keyword, scope);
      out_.subShaders[index].span = {keyword.span.begin, lastEnd_};
    }
    void parsePass(int subShader) {
      const Token &keyword = advance();
      SubShader &owner = out_.subShaders[subShader];
      int index = static_cast<int>(owner.passes.size());
      owner.passes.emplace_back();
      owner.passes.back().keyword = keyword.span;
      size_t scope = 0;
      if (!openBrace(keyword, ScopeKind::Pass, ScopeKind::SubShader, scope))
        return;
      bool first = true;
      bool haveProgram = false;
      while (!atEnd() && peek().kind != Tok::RBrace) {
        const Token &token = peek();
        Pass &pass = out_.subShaders[subShader].passes[index];
        if (token.kind == Tok::CodeBlock) {
          recordBlock(token, subShader, index);
          if (isProgramBlock(token.block)) {
            if (haveProgram)
              report(out_.blocks.back().keyword, "A Pass can contain only one shader program block.", Severity::Warning);
            haveProgram = true;
          }
          advance();
        } else if (isWord(token, "PackageRequirements")) {
          if (!first)
            report(token.span, "A PackageRequirements block must come before all other declarations inside the Pass.");
          parsePackageRequirements(ScopeKind::Pass);
        } else if (isWord(token, "Name")) {
          const Token &kw = advance();
          const Token &value = peek();
          if (auto name = expectString(kw, "a pass name string")) {
            pass.name = *name;
            pass.nameSpan = value.span;
          }
        } else if (isWord(token, "Tags")) {
          parseTags(subShader, index);
        } else if (token.kind == Tok::Ident && ref::findCommand(token.text)) {
          parseCommand(pass.commands, ScopeKind::Pass);
        } else if (isWord(token, "LOD")) {
          // Documented for SubShaders, but Unity's own shaders also set it in a Pass.
          const Token &kw = advance();
          if (peek().kind == Tok::Number && parseInteger(peek().text)) {
            advance();
          } else {
            report(kw.span, "Expected an integer LOD value.");
            skipStatement(kw);
          }
        } else if (isWord(token, "UsePass") || isWord(token, "GrabPass") || isWord(token, "Pass")) {
          report(token.span, "'" + std::string(token.text) + "' is only valid inside a SubShader.");
          skipStatement(token);
        } else if (token.kind == Tok::Ident && isLegacyCommand(token.text)) {
          skipLegacyCommand();
        } else if (token.kind == Tok::Ident) {
          report(token.span, "'" + std::string(token.text) + "' is not a ShaderLab command in the current reference.", Severity::Warning);
          skipStatement(token);
        } else {
          reportUnexpected(token, "a Pass block");
          skipStatement(token);
        }
        first = false;
      }
      closeBrace(keyword, scope);
      out_.subShaders[subShader].passes[index].span = {keyword.span.begin, lastEnd_};
    }
    // Skips a legacy command's arguments on its line and the { } block that some of them take.
    void skipLegacyCommand() {
      const Token &name = advance();
      size_t line = lineOf(name);
      while (!atEnd() && lineOf(peek()) == line) {
        const Token &token = peek();
        if (token.kind == Tok::RBrace || token.kind == Tok::LBrace || token.kind == Tok::CodeBlock)
          break;
        if (token.kind == Tok::Ident &&
            (ref::findCommand(token.text) || isScopeKeyword(token.text) || isLegacyCommand(token.text))) {
          break;
        }
        advance();
      }
      if (peek().kind == Tok::LBrace)
        skipBalanced();
    }
    void parsePackageRequirements(ScopeKind parent) {
      const Token &keyword = advance();
      size_t scope = 0;
      if (!openBrace(keyword, ScopeKind::PackageRequirements, parent, scope))
        return;
      while (!atEnd() && peek().kind != Tok::RBrace) {
        const Token &token = peek();
        if (token.kind != Tok::String) {
          report(token.span, "Expected a package requirement string, for example \"com.unity.render-pipelines.universal\".");
          advance();
          continue;
        }
        advance();
        if (peek().kind == Tok::Colon) {
          const Token &colon = advance();
          if (peek().kind == Tok::String) {
            advance();
          } else {
            report(colon.span, "Expected a version restriction string after ':'.");
          }
        }
      }
      closeBrace(keyword, scope);
    }
    void parseGrabPass() {
      const Token &keyword = advance();
      size_t scope = 0;
      if (!openBrace(keyword, ScopeKind::GrabPass, ScopeKind::SubShader, scope))
        return;
      if (peek().kind == Tok::String)
        advance();
      while (!atEnd() && peek().kind != Tok::RBrace) {
        reportUnexpected(peek(), "a GrabPass block");
        advance();
      }
      closeBrace(keyword, scope);
    }
    void parseTags(int subShader, int pass) {
      const Token &keyword = advance();
      size_t scope = 0;
      bool isPass = pass >= 0;
      if (!openBrace(keyword, ScopeKind::Tags, isPass ? ScopeKind::Pass : ScopeKind::SubShader, scope))
        return;
      std::vector<Tag> tags;
      while (!atEnd() && peek().kind != Tok::RBrace) {
        const Token &key = peek();
        if (key.kind != Tok::String) {
          report(key.span, "Tags must be written as \"name\" = \"value\" pairs.");
          advance();
          continue;
        }
        advance();
        if (peek().kind != Tok::Equals) {
          report(key.span, "Expected '=' after tag name.");
          continue;
        }
        advance();
        if (peek().kind != Tok::String) {
          report(key.span, "Expected a tag value string.");
          continue;
        }
        const Token &value = advance();
        tags.push_back({stringValue(key), stringValue(value), key.span, value.span});
        validateTag(tags.back());
      }
      closeBrace(keyword, scope);
      auto &target = isPass ? out_.subShaders[subShader].passes[pass].tags : out_.subShaders[subShader].tags;
      target.insert(target.end(), tags.begin(), tags.end());
    }
    // Tags aren't checked for placement: Unity's own shaders put SubShader tags in Passes and vice versa.
    void validateTag(const Tag &tag) {
      if (iequals(tag.key, "Queue")) {
        std::string_view value = trim(tag.value);
        size_t nameEnd = 0;
        while (nameEnd < value.size() && std::isalpha(static_cast<unsigned char>(value[nameEnd])))
          ++nameEnd;
        std::string_view name = value.substr(0, nameEnd);
        std::string_view rest = trim(value.substr(nameEnd));
        bool offsetOk = rest.empty();
        if (!rest.empty() && (rest[0] == '+' || rest[0] == '-')) {
          std::string_view digits = trim(rest.substr(1));
          offsetOk = !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        }
        bool numeric = !value.empty() && std::all_of(value.begin(), value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if (!numeric && (!ref::find(ref::tagValues("Queue"), name) || !offsetOk)) {
          report(tag.valueSpan, "Invalid Queue value '" + tag.value + "'. Expected '[queue name]' or '[queue name] + [offset]' where the queue name is one of: " + joinNames(ref::tagValues("Queue")) + ".", Severity::Warning);
        }
        return;
      }
      for (std::string_view closed : {"ForceNoShadowCasting", "IgnoreProjector", "PreviewType"}) {
        if (iequals(tag.key, closed) && !ref::find(ref::tagValues(closed), tag.value)) {
          report(tag.valueSpan, "Invalid " + std::string(closed) + " value '" + tag.value + "'. Expected one of: " + joinNames(ref::tagValues(closed)) + ".", Severity::Warning);
        }
      }
    }
    // ---- Commands ------------------------------------------------------------
    std::vector<CommandArg> collectArgs(const Token &name, bool inStencil) {
      std::vector<CommandArg> args;
      size_t line = lineOf(name);
      while (!atEnd() && lineOf(peek()) == line) {
        const Token &token = peek();
        if (token.kind == Tok::Ident) {
          bool nextStatement = inStencil ? ref::find(ref::stencilFields(), token.text) != nullptr
                                         : (ref::findCommand(token.text) || isScopeKeyword(token.text) ||
                                             isLegacyCommand(token.text));
          if (nextStatement)
            break;
          advance();
          args.push_back({std::string(token.text), token.span});
        } else if (token.kind == Tok::Number) {
          advance();
          args.push_back({std::string(token.text), token.span});
        } else if (token.kind == Tok::Comma) {
          advance();
          args.push_back({",", token.span, false, true});
        } else if (token.kind == Tok::LBracket) {
          const Token &open = advance();
          if (peek().kind == Tok::Ident && peek(1).kind == Tok::RBracket) {
            const Token &id = advance();
            const Token &close = advance();
            args.push_back({std::string(id.text), {open.span.begin, close.span.end}, true});
          } else {
            report(open.span, "Expected a material property name inside [ ].");
            while (!atEnd() && lineOf(peek()) == line && peek().kind != Tok::RBracket && peek().kind != Tok::RBrace)
              advance();
            if (peek().kind == Tok::RBracket)
              advance();
          }
        } else {
          break;
        }
      }
      return args;
    }
    void parseCommand(std::vector<Command> &commands, ScopeKind owner) {
      const Token &name = advance();
      Command command;
      command.name = std::string(name.text);
      command.nameSpan = name.span;
      if (iequals(name.text, "Stencil")) {
        parseStencil(name, command, owner);
      } else {
        command.args = collectArgs(name, false);
        validateCommand(command);
      }
      command.span = {name.span.begin, lastEnd_};
      commands.push_back(std::move(command));
    }
    void parseStencil(const Token &keyword, Command &command, ScopeKind owner) {
      size_t scope = 0;
      if (!openBrace(keyword, ScopeKind::Stencil, owner, scope))
        return;
      while (!atEnd() && peek().kind != Tok::RBrace) {
        const Token &token = peek();
        if (token.kind != Tok::Ident) {
          reportUnexpected(token, "a Stencil block");
          advance();
          continue;
        }
        advance();
        Command field;
        field.name = std::string(token.text);
        field.nameSpan = token.span;
        field.args = collectArgs(token, true);
        field.span = {token.span.begin, lastEnd_};
        validateStencilField(field);
        command.children.push_back(std::move(field));
      }
      closeBrace(keyword, scope);
    }
    static std::vector<const CommandArg *> values(const Command &command) {
      std::vector<const CommandArg *> result;
      for (const CommandArg &arg : command.args) {
        if (!arg.comma)
          result.push_back(&arg);
      }
      return result;
    }
    size_t commaCount(const Command &command) const {
      return static_cast<size_t>(std::count_if(command.args.begin(), command.args.end(), [](const CommandArg &a) { return a.comma; }));
    }
    void checkValue(const Command &command, const CommandArg &arg, std::span<const ref::Entry> table, std::string_view what) {
      if (arg.propertyRef || ref::find(table, arg.text))
        return;
      report(arg.span, "Invalid " + std::string(what) + " '" + arg.text + "' for " + command.name + ". Expected one of: " + joinNames(table) + ".");
    }
    void checkInteger(const Command &command, const CommandArg &arg, long min, long max, std::string_view what) {
      if (arg.propertyRef)
        return;
      auto value = parseInteger(arg.text);
      if (!value || *value < min || *value > max) {
        report(arg.span, "Invalid " + std::string(what) + " '" + arg.text + "' for " + command.name + ". Expected an integer from " + std::to_string(min) + " through " + std::to_string(max) + ".");
      }
    }
    bool requireArgCount(const Command &command, size_t count) {
      auto vals = values(command);
      if (vals.size() == count && commaCount(command) == 0)
        return true;
      if (vals.empty()) {
        report(command.nameSpan, "Missing value for " + command.name + ".");
      } else {
        Span span{command.args.front().span.begin, command.args.back().span.end};
        report(span, command.name + " takes exactly " + std::to_string(count) + " value" + (count == 1 ? "" : "s") + ".");
      }
      return false;
    }
    void validateCommand(const Command &command) {
      const ref::Entry *entry = ref::findCommand(command.name);
      if (!entry)
        return;
      std::string_view name = entry->name;
      auto vals = values(command);
      auto single = ref::commandValues(name);
      if (!single.empty() && name != "BlendOp") {
        if (!requireArgCount(command, 1))
          return;
        const CommandArg &arg = *vals[0];
        // The reference lists On/Off for some switches and True/False for others, but Unity accepts either
        // (its own shaders use `ZClip Off` and `Conservative On`), as well as `ZTest Off`.
        bool isSwitch = single.data() == ref::onOff().data() || single.data() == ref::trueFalse().data();
        bool accepted = (isSwitch && (ref::find(ref::onOff(), arg.text) || ref::find(ref::trueFalse(), arg.text))) ||
                        (name == "ZTest" && iequals(arg.text, "Off"));
        if (!accepted)
          checkValue(command, arg, single, "value");
        return;
      }
      if (name == "BlendOp") {
        // "BlendOp <operation>"; a second ", <operation>" for alpha is tolerated.
        bool ok = (vals.size() == 1 && commaCount(command) == 0) ||
                  (command.args.size() == 3 && command.args[1].comma && !command.args[0].comma && !command.args[2].comma);
        if (!ok) {
          requireArgCount(command, 1);
          return;
        }
        for (const CommandArg *arg : vals)
          checkValue(command, *arg, ref::blendOperations(), "blending operation");
        return;
      }
      if (name == "Blend") {
        size_t i = 0;
        if (!command.args.empty() && !command.args[0].comma && !command.args[0].propertyRef &&
            parseInteger(command.args[0].text)) {
          checkInteger(command, command.args[0], 0, 7, "render target");
          i = 1;
        }
        std::vector<std::vector<const CommandArg *>> groups(1);
        for (; i < command.args.size(); ++i) {
          if (command.args[i].comma) {
            groups.emplace_back();
          } else {
            groups.back().push_back(&command.args[i]);
          }
        }
        bool off = groups.size() == 1 && groups[0].size() == 1 && iequals(groups[0][0]->text, "Off");
        bool factors = (groups.size() == 1 || groups.size() == 2) &&
                       std::all_of(groups.begin(), groups.end(), [](const auto &g) { return g.size() == 2; });
        if (off)
          return;
        if (!factors) {
          Span span = command.args.empty() ? command.nameSpan : Span{command.args.front().span.begin, command.args.back().span.end};
          report(span,
            "Invalid Blend syntax. Expected 'Blend Off', 'Blend <source factor> <destination factor>', or "
            "'Blend <source factor RGB> <destination factor RGB>, <source factor alpha> <destination factor alpha>', "
            "optionally preceded by a render target index.");
          return;
        }
        for (const auto &g : groups) {
          for (const CommandArg *arg : g)
            checkValue(command, *arg, ref::blendFactors(), "blend factor");
        }
        return;
      }
      if (name == "ColorMask") {
        if (vals.empty() || vals.size() > 2 || commaCount(command) != 0) {
          report(vals.empty() ? command.nameSpan : Span{command.args.front().span.begin, command.args.back().span.end},
            "Expected 'ColorMask <channels>' or 'ColorMask <channels> <render target>'.");
          return;
        }
        const CommandArg &channels = *vals[0];
        if (!channels.propertyRef && channels.text != "0") {
          std::string seen;
          bool ok = !channels.text.empty();
          for (char c : channels.text) {
            char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (std::string_view("RGBA").find(upper) == std::string_view::npos || seen.find(upper) != std::string::npos)
              ok = false;
            seen.push_back(upper);
          }
          if (!ok) {
            report(channels.span, "Invalid ColorMask channels '" + channels.text + "'. Expected 0, or any combination of R, G, B, and A without spaces.");
          }
        }
        if (vals.size() == 2)
          checkInteger(command, *vals[1], 0, 7, "render target");
        return;
      }
      if (name == "Offset") {
        bool ok = command.args.size() == 3 && !command.args[0].comma && command.args[1].comma && !command.args[2].comma;
        if (!ok) {
          report(command.args.empty() ? command.nameSpan : Span{command.args.front().span.begin, command.args.back().span.end},
            "Expected 'Offset <factor>, <units>'.");
          return;
        }
        for (const CommandArg *arg : values(command)) {
          if (arg->propertyRef)
            continue;
          char *end = nullptr;
          std::strtod(arg->text.c_str(), &end);
          if (end != arg->text.c_str() + arg->text.size())
            report(arg->span, "Expected a float value for Offset.");
        }
        return;
      }
    }
    void validateStencilField(const Command &field) {
      const ref::Entry *entry = ref::find(ref::stencilFields(), field.name);
      if (!entry) {
        report(field.nameSpan, "Unknown Stencil parameter '" + field.name + "'. Expected one of: " + joinNames(ref::stencilFields()) + ".");
        return;
      }
      if (!requireArgCount(field, 1))
        return;
      const CommandArg &value = field.args[0];
      auto table = ref::stencilFieldValues(entry->name);
      if (table.empty()) {
        checkInteger(field, value, 0, 255, "value");
      } else {
        checkValue(field, value, table, istartsWith(entry->name, "Comp") ? "comparison operation" : "stencil operation");
      }
    }
    void validatePropertyReferences() {
      auto check = [&](const Command &command) {
        for (const CommandArg &arg : command.args) {
          if (arg.propertyRef && !out_.findProperty(arg.text)) {
            // A hint only: materials can carry values that aren't declared (HDRP sets several from C#).
            report(arg.span, "Material property '" + arg.text + "' is not declared in the Properties block.", Severity::Hint);
          }
        }
      };
      auto checkAll = [&](const std::vector<Command> &commands) {
        for (const Command &command : commands) {
          check(command);
          for (const Command &child : command.children)
            check(child);
        }
      };
      for (const SubShader &subShader : out_.subShaders) {
        checkAll(subShader.commands);
        for (const Pass &pass : subShader.passes)
          checkAll(pass.commands);
      }
    }
    std::string_view src_;
    std::vector<Token> toks_;
    LineIndex lines_;
    size_t pos_ = 0;
    size_t lastEnd_ = 0;
    ShaderFile out_;
  };
} // namespace
ShaderFile parseShaderLab(std::string_view source) {
  return Parser(source).run();
}
} // namespace sls
